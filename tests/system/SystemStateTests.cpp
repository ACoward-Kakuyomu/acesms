#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "Stage5TestRom.h"
#include "../cpu/Z80TestSupport.h"
#ifdef _WIN32
#include "win32/FmPlugin.h"
#include <filesystem>
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string_view>

namespace {
using namespace smsgg;
RomImage image(MapperKind mapper) {
    auto bytes=test::stage5InputRom();
    if(mapper!=MapperKind::RomOnly) {
        bytes.resize(131072);
        for(unsigned bank=1;bank<8;++bank) std::fill_n(bytes.begin()+bank*16384,16384,static_cast<uint8_t>(bank));
    }
    bytes[1]=0;bytes[2]=8;
    size_t at=0x800;
    auto emit=[&](std::initializer_list<uint8_t> code){for(uint8_t b:code) bytes[at++]=b;};
    auto out=[&](uint8_t port,uint8_t value){emit({0x3e,value,0xd3,port});};
    auto write=[&](uint16_t address,uint8_t value){emit({0x3e,value,0x32,static_cast<uint8_t>(address),static_cast<uint8_t>(address>>8)});};
    out(0x7f,0x83);out(0x7f,1);out(0x7f,0x92);out(0x06,0xa5);
    out(0xf2,3);out(0xf0,0x30);out(0xf1,0x10);out(0xf0,0x10);out(0xf1,0x80);out(0xf0,0x20);out(0xf1,0x19);
    if(mapper==MapperKind::Sega) {write(0xfffe,2);write(0xffff,3);write(0xfffc,8);write(0xa000,0x5a);}
    if(mapper==MapperKind::Codemasters) {write(0x8000,3);write(0x4000,0x82);write(0xa000,0x5a);}
    emit({0xc3,0,1});
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{mapper,mapper==MapperKind::RomOnly?0u:8192u}));
}
bool sameCpu(const Z80State& a,const Z80State& b) {
    return test::sameRegisters(a.registers,b.registers) && a.iff1==b.iff1 && a.iff2==b.iff2 && a.halted==b.halted &&
        a.flagsModified==b.flagsModified && a.ldAir==b.ldAir && a.eiDelay==b.eiDelay && a.interruptMode==b.interruptMode && a.memptr==b.memptr;
}
bool samePixels(const EmulatorSystem& a,const EmulatorSystem& b) {
    return std::equal(a.framePixels().begin(),a.framePixels().end(),b.framePixels().begin(),[](Rgba8 x,Rgba8 y){return x.r==y.r && x.g==y.g && x.b==y.b && x.a==y.a;});
}
bool compare(EmulatorSystem& original,EmulatorSystem& restored,unsigned count) {
    if(original.tstates()!=restored.tstates() || original.completedFrames()!=restored.completedFrames() || !samePixels(original,restored)) return false;
    const auto firstFrame=original.completedFrames();
    for(unsigned n=0;n<count;++n) {
        const auto previousFrame=original.completedFrames();
        const PadState pad{.up=n%29==0,.right=n%31==0,.button1=n%7<3,.start=n%5==0};
        if(original.step(pad,n==31 || n==300)!=restored.step(pad,n==31 || n==300) ||
            original.tstates()!=restored.tstates() || original.completedFrames()!=restored.completedFrames() ||
            !sameCpu(original.cpu().captureState(),restored.cpu().captureState())) return false;
        const auto a=original.audioFrames(),b=restored.audioFrames();
        if(a.size()!=b.size()) return false;
        for(size_t i=0;i<a.size();++i) if(a[i].left!=b[i].left || a[i].right!=b[i].right) return false;
        if(previousFrame!=original.completedFrames() && !samePixels(original,restored)) return false;
        if(n==0 || n+1==count) {
            if(!samePixels(original,restored)) return false;
            for(unsigned address=0;address<65536;++address)
                if(original.readMemory(static_cast<uint16_t>(address))!=restored.readMemory(static_cast<uint16_t>(address))) return false;
        }
    }
    return count<20000 || original.completedFrames()>firstFrame;
}
bool replay(VideoStandard standard,Machine machine,uint32_t rate,MapperKind mapper,bool fm,FmInterpolation method,const FmBackendPtr& backend) {
    const auto rom=image(mapper);
    auto make=[&]{return std::make_unique<EmulatorSystem>(rom,standard,rate,machine,fm,method,backend);};
    auto source=make(),candidate=make();
    while(source->tstates()<static_cast<uint64_t>(standard==VideoStandard::Ntsc?262:313)*228+7003) source->step();
    auto saved=std::make_unique<EmulatorSystemState>();source->captureState(*saved);
    if(!saved->vdp.frames || saved->tstates!=source->tstates() || saved->audio.time!=saved->tstates) return false;
    if(mapper!=MapperKind::RomOnly && (saved->cartridge.ram[0]!=0x5a || source->readMemory(0xa000)!=0x5a)) return false;
    while(candidate->audioFrames().empty()) candidate->step();
    if(candidate->restoreState(*saved)!=ACESMS_FM_OK || !candidate->audioFrames().empty()) return false;
    const auto cut=saved->tstates;
    if(!compare(*source,*candidate,20000) || saved->tstates!=cut) return false;
    source->captureState(*saved);source.reset();
    auto afterSource=make();
    return afterSource->restoreState(*saved)==ACESMS_FM_OK && afterSource->audioFrames().empty() && compare(*candidate,*afterSource,20000);
}
bool profiles() {
    for(auto mapper:{MapperKind::RomOnly,MapperKind::Sega,MapperKind::Codemasters})
    for(uint32_t rate:{44100u,48000u,96000u})
    for(unsigned profile=0;profile<3;++profile)
        if(!replay(profile==1?VideoStandard::Pal:VideoStandard::Ntsc,profile==2?Machine::GameGear:Machine::Sms,
            rate,mapper,false,FmInterpolation::Sinc,{})) return false;
    return true;
}
bool fm(const FmBackendPtr& backend) {
    for(auto mapper:{MapperKind::RomOnly,MapperKind::Sega,MapperKind::Codemasters})
    for(uint32_t rate:{44100u,48000u,96000u})
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear})
        if(!replay(VideoStandard::Ntsc,Machine::Sms,rate,mapper,true,method,backend)) return false;
    return true;
}
bool failure(const FmBackendPtr& backend) {
    const auto rom=image(MapperKind::Sega);
    auto make=[&]{return std::make_unique<EmulatorSystem>(rom,VideoStandard::Ntsc,48000,Machine::Sms,true,FmInterpolation::Sinc,backend);};
    auto source=make(),candidate=make(),baseline=make();
    while(source->tstates()<70003) source->step();
    auto bad=std::make_unique<EmulatorSystemState>();source->captureState(*bad);bad->audio.fm->backend.pop_back();
    while(candidate->audioFrames().empty()) {candidate->step();baseline->step();}
    const auto delivered=candidate->audioFrames().size();
    return candidate->restoreState(*bad)!=ACESMS_FM_OK && candidate->audioFrames().size()==delivered && compare(*candidate,*baseline,2000);
}
bool latches() {
    std::vector<uint8_t> bytes(8192);
    constexpr std::array<uint8_t,15> code{0x3e,0x55,0xd3,0x3f,0x3e,0xa7,0x32,0x00,0xc0,0x3e,0x14,0xd3,0x3e,0x76,0};
    std::copy(code.begin(),code.end(),bytes.begin());
    const auto rom=std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::RomOnly}));
    auto original=std::make_unique<EmulatorSystem>(rom,VideoStandard::Ntsc,48000),candidate=std::make_unique<EmulatorSystem>(rom,VideoStandard::Ntsc,48000);
    for(unsigned n=0;n<9;++n) original->step({.button1=true,.start=true});
    auto state=std::make_unique<EmulatorSystemState>();original->captureState(*state);
    if(state->memory.ramEnabled || !state->memory.cartridgeEnabled || state->memory.ram[0]!=0xa7 || !state->memoryControl.ioDisabled || state->controller.control!=0x55) return false;
    if(candidate->restoreState(*state)!=ACESMS_FM_OK || candidate->readMemory(0xc000)!=0xff) return false;
    candidate->captureState(*state);
    return state->memory.ram[0]==0xa7 && !state->memory.ramEnabled && state->memoryControl.ioDisabled && state->controller.control==0x55 && state->controller.pad.button1 && state->controller.pad.start;
}
}
int main(int argc,char** argv) {
    if(argc<2) return 1;
    FmBackendPtr backend;
#ifdef _WIN32
    if(argc==3) {
        const auto dll=std::filesystem::absolute(argv[2]);auto loaded=loadFmPlugin(dll.parent_path(),dll.filename());
        if(const auto* error=std::get_if<std::string>(&loaded)) {std::cerr<<*error;return 1;}
        backend=std::get<FmBackendPtr>(std::move(loaded));
    }
#endif
    const std::string_view operation=argv[1];
    const bool passed=operation=="profiles"?profiles():operation=="fm"?fm(backend):operation=="failure"?failure(backend):operation=="latches"&&latches();
    if(!passed) std::cerr<<"System state failed: "<<operation<<'\n';return passed?0:1;
}
