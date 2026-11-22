#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#ifdef _WIN32
#include "win32/FmPlugin.h"
#include <filesystem>
#endif

int main(int argc,char** argv) {
    using namespace smsgg;
    FmBackendPtr backend;
#ifdef _WIN32
    if(argc==2) {
        const auto dll=std::filesystem::absolute(argv[1]);
        auto result=loadFmPlugin(dll.parent_path(),dll.filename());
        if(const auto* error=std::get_if<std::string>(&result)) {std::cerr<<*error;return 1;}
        backend=std::get<FmBackendPtr>(std::move(result));
    }
#endif
    struct FmCase {
        Machine machine;
        VideoStandard video;
        bool effective;
    };
    constexpr FmCase configurations[]{
        {Machine::GameGear,VideoStandard::Ntsc,false},
        {Machine::Sms,VideoStandard::Pal,false},
        {Machine::Sg1000,VideoStandard::Ntsc,false},
        {Machine::Sms,VideoStandard::Ntsc,true},
    };
    constexpr std::array<uint8_t,16> program{
        0x3e,1,0xd3,0xf2,0xd3,0xf0,0xd3,0xf1,0xdb,0xf2,0x32,0x00,0xc0,0x76,0,0,
    };
    for(const auto [machine,video,effective]:configurations) {
        std::vector<uint8_t> rom(8192);
        std::ranges::copy(program,rom.begin());
        auto image=std::get<RomImage>(RomLoader::fromBytes(std::move(rom),{MapperKind::RomOnly,0,CopierHeader::None,machine}));
        auto system=std::make_unique<EmulatorSystem>(std::move(image),video,48000,machine,true,FmInterpolation::Sinc,backend);
        for(int instruction=0;instruction<7;++instruction) if(!std::holds_alternative<uint32_t>(system->step())) return 1;
        auto state=std::make_unique<EmulatorSystemState>();
        system->captureState(*state);
        if(state->audio.fm.has_value()!=effective || system->readMemory(0xc000)!=(effective?1:0xff)) {
            std::cerr<<"Incorrect FM availability or port decoding for machine "<<static_cast<int>(machine)
                <<" and video standard "<<static_cast<int>(video)<<'\n';
            return 1;
        }
    }
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear})
    for(const bool fm : {false,true}) for(const uint32_t rate : {44100u,48000u,96000u}) {
        std::vector<uint8_t> rom(8192);
        size_t at=0;
        std::map<size_t,std::pair<uint8_t,uint8_t>> writes;
        auto emit=[&](std::initializer_list<uint8_t> code){for(auto byte:code) rom[at++]=byte;};
        auto out=[&](uint8_t port,uint8_t value) {
            emit({0x3e,value});writes.emplace(at,std::pair{port,value});emit({0xd3,port});
        };
        out(0x3e,4);
        for(uint8_t i=0;i<8;++i) {
            out(0xf2,i&1);emit({0xdb,0xf2,0x32,i,0xc0});
        }
        out(0x3e,0);out(0x7f,0x90);out(0xf2,3);
        out(0xf0,0x30);out(0xf1,0x10);out(0xf0,0x10);out(0xf1,0x80);out(0xf0,0x20);out(0xf1,0x19);
        emit({0x76});
        auto system=std::make_unique<EmulatorSystem>(std::get<RomImage>(RomLoader::fromBytes(rom,{MapperKind::RomOnly})),
            VideoStandard::Ntsc,rate,Machine::Sms,fm,method,backend);
        AudioMixer expected(3579545,rate,fm,method,backend);
        std::vector<AudioFrame> buffer(16);
        uint64_t count=0;bool audible=false;
        while(system->tstates()<200000) {
            const auto write=writes.find(system->cpu().registers().pc);
            if(write!=writes.end()) {
                const auto [port,value]=write->second;
                if(port==0x7f) expected.writePsg(value);
                if(fm && (port==0xf0 || port==0xf1)) expected.writeFm(port&1,value);
                if(fm && port==0xf2) expected.writeFmControl(value);
            }
            if(!std::holds_alternative<uint32_t>(system->step())) return 1;
            const auto n=expected.advanceTo(system->tstates(),buffer);
            if(n!=system->audioFrames().size()) return 1;
            for(size_t i=0;i<n;++i) {
                const auto frame=system->audioFrames()[i];
                if(std::abs(frame.left-buffer[i].left)>1e-6 || frame.left!=frame.right) return 1;
                audible|=frame.left!=0;
            }
            count+=n;
            if(count!=system->tstates()*rate/3579545) return 1;
        }
        for(uint8_t i=0;i<8;++i) if(system->readMemory(0xc000+i)!=(fm?(i&1):0xff)) return 1;
        if(!audible) return 1;
    }
    return 0;
}
