#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "../bus/IoTestSupport.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <iostream>
#include <memory>
#include <string_view>

namespace {
smsgg::RomImage image() {
    std::vector<uint8_t> rom(8192);rom[0]=0x76;
    return std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(rom),{smsgg::MapperKind::RomOnly}));
}
bool machine() {
    auto sms=std::make_unique<smsgg::EmulatorSystem>(image(),smsgg::VideoStandard::Ntsc,48000);
    auto gg=std::make_unique<smsgg::EmulatorSystem>(image(),smsgg::VideoStandard::Ntsc,48000,smsgg::Machine::GameGear);
    const auto a=sms->displayArea(),b=gg->displayArea();
    if(a.x || a.y || a.width!=256 || a.height!=192 || b.x!=48 || b.y!=24 || b.width!=160 || b.height!=144) return false;
    if(std::get<uint32_t>(sms->step({},true))!=11 || sms->cpu().registers().pc!=0x66) return false;
    uint64_t samples=0;
    for(unsigned i=0;i<10000;++i) {
        if(std::get<uint32_t>(gg->step({},true))!=4 || gg->cpu().registers().pc!=1) return false;
        samples+=gg->audioFrames().size();
        for(auto f:gg->audioFrames()) if(f.left || f.right) return false;
    }
    return gg->tstates()==40000 && samples==uint64_t(40000)*48000/3579545 && gg->completedFrames()==0;
}
bool ports() {
    smsgg::test::Io devices;
    smsgg::IoBus io(devices,smsgg::Machine::GameGear);
    for(unsigned port=0;port<65536;++port) {
        const unsigned low=port&255;
        const int read=low<7?int(low):low<64?-1:low<128?0x7e+(low&1):low<192?0xbe+(low&1)
            :low==0xc0 || low==0xdc?0xdc:low==0xc1 || low==0xdd?0xdd:-1;
        const int write=low<7?int(low):low<64?0x3e+(low&1):low<128?0x7f:low<192?0xbe+(low&1):-1;
        devices.reply=0xa5;devices.accesses.clear();
        if(io.readPort(static_cast<uint16_t>(port))!=(read<0?0xff:0xa5)
            || devices.accesses.size()!=(read<0?0:1)) return false;
        if(read>=0 && (devices.accesses[0].port!=read || devices.accesses[0].write)) return false;
        devices.accesses.clear();io.writePort(static_cast<uint16_t>(port),0x69);
        if(devices.accesses.size()!=(write<0?0:1)) return false;
        if(write>=0 && (devices.accesses[0].port!=write || !devices.accesses[0].write || devices.accesses[0].value!=0x69)) return false;
    }
    smsgg::test::Memory memory;
    for(unsigned high=0;high<256;++high) for(uint8_t low:{0,6,7,8,0xc0,0xc2,0xdc,0xdd,0xde}) {
        smsgg::Z80 cpu({.af=0x6900,.bc=static_cast<uint16_t>(high*256+low)});
        devices.reply=0xa5;devices.accesses.clear();
        const std::array<uint8_t,2> out{0xed,0x79},in{0xed,0x78};
        if(!cpu.executeInstruction(out,&memory,&io)) return false;
        const bool readable=low<7 || low==0xc0 || low==0xdc || low==0xdd;
        if(!cpu.executeInstruction(in,&memory,&io) || cpu.registers().a()!=(readable?0xa5:0xff)) return false;
    }
    return true;
}
bool inputs() {
    smsgg::Controller controller;
    smsgg::AudioMixer audio(3579545,48000);
    smsgg::Vdp vdp(smsgg::VideoStandard::Ntsc,smsgg::Machine::GameGear);
    smsgg::SmsIo terminal(controller,audio,vdp,smsgg::Machine::GameGear);
    smsgg::test::Memory external;external.bytes[0x4000]=0xa5;
    smsgg::MemoryBus memory(external);
    smsgg::MemoryControl control(memory,terminal,smsgg::Machine::GameGear);
    smsgg::IoBus io(control,smsgg::Machine::GameGear);
    constexpr std::array<uint8_t,7> initial{0xc0,0x7f,0xff,0,0xff,0,0xff};
    for(unsigned port=0;port<7;++port) {
        if(io.readPort(static_cast<uint16_t>(port))!=initial[port]) return false;
        io.writePort(static_cast<uint16_t>(port),0xff);
        if(io.readPort(static_cast<uint16_t>(port))!=initial[port] || memory.read8(0x4000)!=0xa5) return false;
    }
    for(unsigned bits=0;bits<128;++bits) {
        controller.setState({bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8),bool(bits&16),bool(bits&32),bool(bits&64)});
        if(io.readPort(0)!=((bits&64)?0x40:0xc0) || io.readPort(0xdc)!=(0xff^(bits&63))) return false;
        for(unsigned value=0;value<256;++value) {
            io.writePort(0x3f,static_cast<uint8_t>(value));
            const unsigned dc=(0xdf^(bits&31)) | ((value&1?!(bits&32):bool(value&16))?32:0);
            const unsigned dd=0xbf | ((value&2 || value&32)?64:0);
            if(io.readPort(0xdc)!=dc || io.readPort(0xdd)!=dd) return false;
        }
        io.writePort(0x3f,0xff);
    }
    memory.write8(0xc000,0x56);
    for(unsigned value=0;value<256;++value) {
        io.writePort(0x3e,static_cast<uint8_t>(value));
        if(memory.read8(0x4000)!=0xa5 || memory.read8(0xc000)!=((value&16)?0xff:0x56)) return false;
    }
    std::vector<uint8_t> rom(8192);
    const std::array<uint8_t,13> program{0xdb,0,0x32,0,0xc0,0xdb,0xdc,0x32,1,0xc0,0xc3,0,0};
    std::copy(program.begin(),program.end(),rom.begin());
    auto system=std::make_unique<smsgg::EmulatorSystem>(
        std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(rom),{smsgg::MapperKind::RomOnly})),
        smsgg::VideoStandard::Ntsc,48000,smsgg::Machine::GameGear);
    for(bool pressed:{true,true,false,false}) {
        for(unsigned i=0;i<40;++i) system->step({.button1=pressed,.start=pressed},true);
        if(system->readMemory(0xc000)!=(pressed?0x40:0xc0) || system->readMemory(0xc001)!=(pressed?0xef:0xff)) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    const auto name=argc==2?std::string_view(argv[1]):"";
    const bool pass=name=="machine"?machine():name=="ports"?ports():name=="inputs"&&inputs();
    if(!pass) std::cerr<<"Game Gear test failed: "<<name<<'\n';
    return pass?0:1;
}
