#include "core/audio/AudioMixer.h"
#ifdef _WIN32
#include "win32/FmPlugin.h"
#include <filesystem>
#endif

#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;
void prepare(AudioMixer& mixer,bool fm) {
    for(uint8_t value:{0x83,0x01,0x92,0xe7,0xf3}) mixer.writePsg(value);
    mixer.writeStereo(0xa5);
    if(fm) {
        mixer.writeFmControl(3);
        for(auto [reg,value]:std::array<std::array<uint8_t,2>,3>{{{0x30,0x10},{0x10,0x80},{0x20,0x19}}}) {
            mixer.writeFm(0,reg);mixer.writeFm(1,value);
        }
    }
}
bool same(AudioFrame a,AudioFrame b) {return a.left==b.left && a.right==b.right;}
bool partial() {
    AudioMixer original(1600,100),restored(1600,100);
    original.writePsg(0x90);
    std::array<AudioFrame,2> a{},b{};
    if(original.advanceTo(8,a)) return false;
    const auto saved=original.captureState();
    if(saved.time!=8 || saved.interval!=8 || saved.area.left!=2 || saved.area.right!=2 || saved.clock.phase!=800 || saved.fm) return false;
    restored.advanceTo(28,b);
    if(restored.restoreState(saved)!=ACESMS_FM_OK) return false;
    original.writePsg(0x9f);restored.writePsg(0x9f);
    return original.advanceTo(16,a)==1 && restored.advanceTo(16,b)==1 && a[0].left==.125f && same(a[0],b[0]);
}
bool continuation(const FmBackendPtr& backend) {
    for(bool fm:{false,true})
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear})
    for(uint32_t rate:{44100u,48000u,96000u})
    for(uint64_t cut:{1ull,8280ull,140003ull}) {
        AudioMixer original(3579545,rate,fm,method,backend),restored(3579545,rate,fm,method,backend);
        prepare(original,fm);
        std::array<AudioFrame,4000> prefix{};original.advanceTo(cut,prefix);
        const auto saved=original.captureState();
        if(saved.time!=cut || bool(saved.fm)!=fm) return false;
        if(fm && saved.fm->backend!=original.captureState().fm->backend) return false;
        if(restored.restoreState(saved)!=ACESMS_FM_OK) return false;
        for(uint64_t n=1;n<=2048;++n) {
            if(fm && n%257==0) {original.writeFmControl(static_cast<uint8_t>((n/257)%4));restored.writeFmControl(static_cast<uint8_t>((n/257)%4));}
            const auto target=((cut*rate/3579545+n)*3579545+rate-1)/rate;
            std::array<AudioFrame,1> a{},b{};
            if(original.advanceTo(target,a)!=1 || restored.advanceTo(target,b)!=1 || !same(a[0],b[0])) return false;
        }
        if(saved.time!=cut) return false;
    }
    return true;
}
bool failure(const FmBackendPtr& backend) {
    AudioMixer source(3579545,48000,true,FmInterpolation::Sinc,backend),candidate(3579545,48000,true,FmInterpolation::Sinc,backend),baseline(3579545,48000,true,FmInterpolation::Sinc,backend);
    prepare(source,true);std::array<AudioFrame,2000> buffer{};source.advanceTo(140003,buffer);
    auto bad=source.captureState();bad.fm->backend.pop_back();
    const auto before=candidate.captureState();
    if(candidate.restoreState(bad)==ACESMS_FM_OK) return false;
    const auto after=candidate.captureState();
    if(after.time!=before.time || after.clock.phase!=before.clock.phase || after.fm->backend!=before.fm->backend) return false;
    std::array<AudioFrame,2000> expected{};
    const auto n=candidate.advanceTo(140003,buffer);
    if(baseline.advanceTo(140003,expected)!=n) return false;
    for(size_t i=0;i<n;++i) if(!same(buffer[i],expected[i])) return false;
    return true;
}
}
int main(int argc,char** argv) {
    if(argc<2) return 1;
    FmBackendPtr backend;
#ifdef _WIN32
    if(argc==3) {
        const auto dll=std::filesystem::absolute(argv[2]);
        auto loaded=loadFmPlugin(dll.parent_path(),dll.filename());
        if(const auto* error=std::get_if<std::string>(&loaded)) {std::cerr<<*error;return 1;}
        backend=std::get<FmBackendPtr>(std::move(loaded));
    }
#endif
    const std::string_view name=argv[1];
    const bool passed=name=="partial"?partial():name=="continuation"?continuation(backend):name=="failure"&&failure(backend);
    if(!passed) std::cerr<<"Mixer state failed: "<<name<<'\n';return passed?0:1;
}
