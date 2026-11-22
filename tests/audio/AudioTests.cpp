#include "core/audio/AudioMixer.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
bool near(double a,double b) { return std::abs(a-b)<1e-6; }
bool clockTest() {
    for (const uint32_t c : {3579545u,3546893u}) for (const uint32_t r : {44100u,48000u,96000u}) {
        smsgg::AudioClock clock(c,r);
        uint64_t time=0,total=0;
        for (uint64_t n=1;n<=200;++n) {
            const auto due=(n*c+r-1)/r;
            const auto before=due-time-1;
            if (clock.advance(before)!=0 || clock.tstatesToSample()!=1 || clock.advance(1)!=1) return false;
            time=due;
        }
        smsgg::AudioClock bulk(c,r),split(c,r);
        const uint64_t longTime=uint64_t(c)*3600+12345;
        const auto expected=uint64_t(r)*3600+uint64_t(12345)*r/c;
        if (bulk.advance(longTime)!=expected) return false;
        for (uint64_t t=0;t<longTime;) {
            const auto d=std::min<uint64_t>(longTime-t,1234567);
            total+=split.advance(d); t+=d;
        }
        if (total!=expected || bulk.tstatesToSample()!=split.tstatesToSample()) return false;
        if (r==48000) {
            smsgg::AudioClock frames(c,r);
            const unsigned lines=c==3579545?262:313;
            const std::array<uint64_t,3> expectedFrames=c==3579545
                ? std::array<uint64_t,3>{801,1602,2403}:std::array<uint64_t,3>{965,1931,2897};
            uint64_t framesTotal=0;
            for(unsigned i=0;i<3;++i) { framesTotal+=frames.advance(lines*228); if(framesTotal!=expectedFrames[i]) return false; }
        }
    }
    return true;
}
bool mixer() {
    smsgg::AudioMixer m(1600,100);
    std::array<smsgg::AudioFrame,8> out{};
    if (m.advanceTo(15,out)!=0 || m.advanceTo(16,out)!=1 || out[0].left!=0) return false;
    m.writePsg(0x90); // +0.25 constant tone, half an output sample.
    if(m.advanceTo(24,out)!=0) return false;
    m.writePsg(0x9f);
    if(m.advanceTo(32,out)!=1 || !near(out[0].left,.125) || out[0].right!=out[0].left) return false;
    if(m.advanceTo(48,out)!=1 || out[0].left!=0) return false;
    m.writePsg(0x90); m.writePsg(0x9f); // Same-time write order, no elapsed audio between them.
    if(m.advanceTo(64,out)!=1 || out[0].left!=0) return false;
    smsgg::AudioMixer tone(1600,100);
    tone.writePsg(0x83); tone.writePsg(0); tone.writePsg(0x90);
    if(tone.advanceTo(96,out)!=6) return false;
    constexpr std::array<double,6> expected{-.25,.25,.25,.25,-.25,-.25};
    for(unsigned i=0;i<6;++i) if(!near(out[i].left,expected[i]) || out[i].left!=out[i].right) return false;
    return true;
}
bool partition() {
    for(const uint32_t c : {3579545u,3546893u}) for(const uint32_t r : {44100u,48000u,96000u}) {
        smsgg::AudioMixer a(c,r),b(c,r);
        for(auto* m : {&a,&b}) {m->writePsg(0x8e);m->writePsg(0x0f);m->writePsg(0x90);m->writePsg(0xe4);m->writePsg(0xf4);}
        std::vector<smsgg::AudioFrame> whole(r), parts(r);
        const auto n=a.advanceTo(c,whole);
        size_t count=0;
        for(uint64_t t=0;t<c;) {t=std::min<uint64_t>(c,t+37);count+=b.advanceTo(t,std::span(parts).subspan(count));}
        if(n!=r || count!=r) return false;
        bool changed=false;
        for(size_t i=0;i<n;++i) {
            if(!near(whole[i].left,parts[i].left) || whole[i].left!=whole[i].right
                || !std::isfinite(whole[i].left) || std::abs(whole[i].left)>1) return false;
            if(i && !near(whole[i].left,whole[i-1].left)) changed=true;
        }
        if(!changed) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="clock"?clockTest():name=="mixer"?mixer():name=="partition"&&partition();
    if(!pass) std::cerr<<"Audio test failed: "<<name<<'\n';
    return pass?0:1;
}
