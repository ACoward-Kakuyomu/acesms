#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "core/audio/AudioMixer.h"
#include "Stage5TestRom.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
smsgg::RomImage image(std::vector<uint8_t> bytes,smsgg::MapperKind mapper=smsgg::MapperKind::RomOnly) {
    return std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(bytes),{mapper}));
}
bool bus() {
    smsgg::test::Memory cartridge;
    smsgg::MemoryBus memory(cartridge);
    smsgg::Controller pad;
    smsgg::AudioMixer audio(1600,100);
    smsgg::Vdp vdp(smsgg::VideoStandard::Ntsc);
    smsgg::SmsIo terminal(pad,audio,vdp);
    smsgg::VdpIo video(vdp,terminal);
    smsgg::MemoryControl control(memory,video);
    smsgg::IoBus io(control);
    std::array<smsgg::AudioFrame,1> out{};
    uint64_t t=0;
    for(unsigned high=0;high<256;++high) for(unsigned low=0x40;low<0x80;++low) {
        io.writePort(static_cast<uint16_t>((high<<8)|low),0x90);
        if(audio.advanceTo(t+=16,out)!=1 || out[0].left!=.25f) return false;
        io.writePort(static_cast<uint16_t>((high<<8)|low),0x9f);
        if(audio.advanceTo(t+=16,out)!=1 || out[0].left!=0) return false;
        if(io.readPort(static_cast<uint16_t>(high<<8|0x7e))!=vdp.readVCounter()) return false;
    }
    io.writePort(0x3f,0xff);
    return io.readPort(0xdc)==0xff && io.readPort(0xdd)==0xff && io.readPort(0x7f)==vdp.readHCounter();
}
bool clockProgression() {
    for(const auto standard : {smsgg::VideoStandard::Ntsc,smsgg::VideoStandard::Pal}) {
        const uint32_t c=standard==smsgg::VideoStandard::Ntsc?3579545:3546893;
        for(const uint32_t r : {44100u,48000u,96000u}) {
            std::vector<uint8_t> rom(8192);
            // Every OUT starts at 7T; the sample interval containing it is partly silent.
            const std::array<uint8_t,7> code{0x3e,0x90,0xd3,0x40,0x76,0,0};
            std::copy(code.begin(),code.end(),rom.begin());
            auto system=std::make_unique<smsgg::EmulatorSystem>(image(rom),standard,r);
            uint64_t n=0; double area=0; uint64_t lastBoundary=0;
            for(unsigned i=0;i<30000;++i) {
                const auto before=system->tstates();
                const auto elapsed=std::get<uint32_t>(system->step());
                if(elapsed>262204 || system->tstates()!=before+elapsed) return false;
                for(const auto frame : system->audioFrames()) {
                    ++n;
                    const auto boundary=(n*c+r-1)/r;
                    const double expected=n==1 ? .25*double(boundary-7)/double(boundary):.25;
                    if(std::abs(frame.left-expected)>1e-6 || frame.left!=frame.right) return false;
                    area+=frame.left*double(boundary-lastBoundary);
                    lastBoundary=boundary;
                }
                if(n!=system->tstates()*r/c) return false;
            }
            if(std::abs(area-.25*double(lastBoundary-7))>.001) return false;
            const auto time=system->tstates();
            if(std::get<uint32_t>(system->step({},true))!=11) return false;
            n+=system->audioFrames().size();
            if(n!=(time+11)*r/c) return false;
        }
    }
    return true;
}
bool prefix() {
    std::vector<uint8_t> bytes(65536,0xdd); bytes[0xbfff]=0;
    auto system=std::make_unique<smsgg::EmulatorSystem>(image(bytes,smsgg::MapperKind::Sega),smsgg::VideoStandard::Pal,96000);
    if(std::get<uint32_t>(system->step())!=196608 || system->audioFrames().size()!=uint64_t(196608)*96000/3546893) return false;
    for(const auto p : system->audioFrames()) if(p.left || p.right) return false;
    auto failing=std::make_unique<smsgg::EmulatorSystem>(image(smsgg::test::stage5PrefixRom(),smsgg::MapperKind::Sega),smsgg::VideoStandard::Ntsc,48000);
    for(unsigned i=0;i<50000;++i) {
        const auto time=failing->tstates();
        if(std::holds_alternative<smsgg::CpuStepError>(failing->step()))
            return failing->tstates()==time && failing->audioFrames().empty();
    }
    return false;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="bus"?bus():name=="clock"?clockProgression():name=="prefix"&&prefix();
    if(!pass) std::cerr<<"System audio test failed: "<<name<<'\n';
    return pass?0:1;
}
