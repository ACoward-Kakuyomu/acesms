#include "core/audio/AudioMixer.h"
#include "core/audio/FmResampler.h"
#include "core/bus/IoBus.h"
#include "core/bus/MemoryControl.h"
#include "core/bus/SmsIo.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>
#include <emu2413.h>

namespace {
using namespace smsgg;
bool near(float a, float b) { return std::abs(a-b) < 1e-6f; }
constexpr std::array methods{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear};
// Independent native clock scheduling, using the fixed upstream directly.
struct Reference {
    std::unique_ptr<OPLL,decltype(&OPLL_delete)> chip{OPLL_new(72,1),OPLL_delete};
    FmResampler converter;
    uint64_t native{};
    explicit Reference(uint32_t rate,FmInterpolation method=FmInterpolation::Sinc) : converter(3579545,rate,method) {}
    void writeIo(uint8_t port,uint8_t value) {OPLL_writeIO(chip.get(),port,value);}
    void advanceTo(uint64_t time) {while(native<time/72) {++native;converter.push(OPLL_calc(chip.get()));}}
    double sample(uint64_t time) {advanceTo(time);return converter.sample()/32768;}
};
template<class Write> void tone(Write write) {
    write(0x30,0x10); write(0x10,0x80); write(0x20,0x19);
}
bool mix() {
    for(auto method:methods) for (const uint32_t rate : {44100u,48000u,96000u}) {
        AudioMixer mixer(3579545,rate,true,method), psg(3579545,rate);
        Reference reference(rate,method);
        tone([&](uint8_t r,uint8_t v) {
            mixer.writeFm(0,r); mixer.writeFm(1,v);
            reference.writeIo(0,r); reference.writeIo(1,v);
        });
        for (auto* m : {&mixer,&psg}) {m->writePsg(0x83);m->writePsg(0);m->writePsg(0x90);}
        std::array<AudioFrame,1> actual{}, pure{};
        bool fmAudible=false;
        for (uint64_t n=1;n<=16000;++n) {
            const uint8_t mode=static_cast<uint8_t>(((n-1)/1000)%4);
            mixer.writeFmControl(mode | 0xfc);
            if(mixer.readFmControl()!=(mode&1)) return false;
            const uint64_t target=(n*3579545+rate-1)/rate;
            if(mixer.advanceTo(target,actual)!=1 || psg.advanceTo(target,pure)!=1) return false;
            const float fm=static_cast<float>(reference.sample(target));
            const float expected=(mode==0 || mode==3 ? pure[0].left*.5f : 0)+(mode&1 ? fm*.5f : 0);
            if(!near(actual[0].left,expected) || actual[0].left!=actual[0].right || !std::isfinite(actual[0].left)
                || std::abs(actual[0].left)>1) return false;
            fmAudible |= mode==1 && fm!=0;
        }
        if(!fmAudible) return false;
    }
    return true;
}
bool partition() {
    for(auto method:methods) for(const uint32_t rate : {44100u,48000u,96000u}) {
        AudioMixer whole(3579545,rate,true,method), split(3579545,rate,true,method);
        for(auto* m : {&whole,&split}) {
            tone([&](uint8_t r,uint8_t v){m->writeFm(0,r);m->writeFm(1,v);});
            m->writeFmControl(3);m->writePsg(0x90);
        }
        uint64_t time=0;
        for(const uint64_t target : {uint64_t(3579545/rate)-1,uint64_t(3579545/rate)+1,10001ull,3579545ull}) {
            std::vector<AudioFrame> a(rate+1),b(rate+1);
            const auto count=whole.advanceTo(target,a);
            size_t n=0;
            for(uint64_t t=time;t<target;) {t=std::min(target,t+37);n+=split.advanceTo(t,std::span(b).subspan(n));}
            if(n!=count) return false;
            for(size_t i=0;i<n;++i) if(!near(a[i].left,b[i].left) || a[i].right!=b[i].right) return false;
            for(auto* m : {&whole,&split}) {
                // Same-time data ordering must retain the last register write.
                m->writeFm(0,0x10);m->writeFm(1,0x40);m->writeFm(1,0x80);
                m->writeFmControl(target==10001 ? 2 : 3);
            }
            time=target;
        }
    }
    AudioMixer partial(3579545,223721,true);
    std::array<AudioFrame,1> out{};
    partial.writePsg(0x90);
    if(partial.advanceTo(8,out)!=0) return false;
    partial.writeFmControl(2);
    if(partial.advanceTo(17,out)!=1 || !near(out[0].left,8.f/17*.25f*.5f)) return false;
    return true;
}
bool bus() {
    test::Memory cartridge;
    MemoryBus memory(cartridge);
    Controller controller;
    AudioMixer audio(3579545,48000,true);
    SmsIo terminal(controller,audio,Machine::Sms,true);
    MemoryControl control(memory,terminal);
    IoBus io(control,Machine::Sms,true);
    io.writePort(0x3e,4);
    for(unsigned value=0;value<256;++value) {
        io.writePort(0xf2,static_cast<uint8_t>(value));
        if(io.readPort(0xf2)!=(value&1) || io.readPort(0xf0)!=0xff || io.readPort(0xf1)!=0xff) return false;
    }
    io.writePort(0xf2,1);
    tone([&](uint8_t r,uint8_t v){io.writePort(0xabf0,r);io.writePort(0xcdf1,v);});
    Reference reference(48000);
    tone([&](uint8_t r,uint8_t v){reference.writeIo(0,r);reference.writeIo(1,v);});
    std::array<AudioFrame,1> out{};
    for(uint64_t n=1;n<=4096;++n) {
        if(audio.advanceTo((n*3579545+47999)/48000,out)!=1
            || !near(out[0].left,static_cast<float>(reference.sample((n*3579545+47999)/48000)*.5))) return false;
    }
    return true;
}
bool timing() {
    for(auto method:methods) for(uint32_t rate:{44100u,48000u,96000u}) {
        AudioMixer mixer(3579545,rate,true,method);Reference reference(rate,method);
        tone([&](uint8_t r,uint8_t v){mixer.writeFm(0,r);mixer.writeFm(1,v);reference.writeIo(0,r);reference.writeIo(1,v);});
        uint64_t sample=1;uint8_t control=3;mixer.writeFmControl(control);
        std::vector<AudioFrame> out(4096);
        for(uint64_t target:{71ull,72ull,73ull,143ull,144ull,145ull,7199ull,7200ull,7201ull,50040ull,100003ull}) {
            const auto count=mixer.advanceTo(target,out);size_t index=0;
            while((sample*3579545+rate-1)/rate<=target) {
                const auto due=(sample*3579545+rate-1)/rate;
                const double expected=reference.sample(due)*(control&1?.5:0);
                if(index>=count || !near(out[index].left,static_cast<float>(expected)) || out[index].right!=out[index].left) return false;
                ++index;++sample;
            }
            if(count!=index) return false;
            reference.advanceTo(target); // Native updates at target happen before the following writes.
            mixer.writeFm(0,0x10);reference.writeIo(0,0x10);
            mixer.writeFm(1,0x40);reference.writeIo(1,0x40);
            mixer.writeFm(1,static_cast<uint8_t>(target));reference.writeIo(1,static_cast<uint8_t>(target));
            control=static_cast<uint8_t>((target/71)%4);mixer.writeFmControl(control);
        }
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2) {
        const std::string_view name(argv[1]);
        if(name=="mix") return mix()?0:1;
        if(name=="partition") return partition()?0:1;
        if(name=="bus") return bus()?0:1;
        if(name=="timing") return timing()?0:1;
    }
    return 1;
}
