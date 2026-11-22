#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>
#include <tuple>

namespace smsgg {
struct PsgTestAccess {
    static auto timing(const Psg& p) {
        return std::tuple{p.tones_,p.volumes_,p.counters_,p.outputs_,p.selected_,p.noise_,
            p.divider_,p.noiseCounter_,p.noisePhase_,p.lfsr_,p.noiseOutput_};
    }
};
}
using namespace smsgg;
namespace {
bool near(double a,double b) {return std::abs(a-b)<1e-6;}
bool masks() {
    for(unsigned channel=0;channel<4;++channel) for(unsigned mask=0;mask<256;++mask) {
        Psg p;p.write(static_cast<uint8_t>(0x90+(channel<<5)));
        const auto state=PsgTestAccess::timing(p);
        p.writeStereo(static_cast<uint8_t>(mask));
        if(PsgTestAccess::timing(p)!=state) return false;
        const auto area=p.advance(100);
        const double expected=channel==3?-25:25;
        if(!near(area.left,(mask&(0x10<<channel))?expected:0) || !near(area.right,(mask&(1<<channel))?expected:0)) return false;
    }
    return true;
}
bool phase() {
    Psg reference,masked;
    for(auto* p:{&reference,&masked}) {p->write(0x83);p->write(0);p->write(0x90);p->write(0xe4);p->write(0xf2);}
    masked.writeStereo(0);
    for(uint64_t n:{1,31,7,100,100003}) {
        reference.advance(n);const auto silence=masked.advance(n);
        if(silence.left || silence.right || PsgTestAccess::timing(reference)!=PsgTestAccess::timing(masked)) return false;
    }
    masked.writeStereo(0xff);
    const auto a=reference.advance(4000),b=masked.advance(4000);
    if(!near(a.left,b.left) || !near(a.right,b.right)) return false;
    AudioMixer mixer(1600,100);mixer.writePsg(0x90);
    std::array<AudioFrame,2> out{};
    if(mixer.advanceTo(8,out)) return false;
    mixer.writeStereo(0x10);
    if(mixer.advanceTo(16,out)!=1 || !near(out[0].left,.25) || !near(out[0].right,.125)) return false;
    if(mixer.advanceTo(32,out)!=1 || !near(out[0].left,.25) || out[0].right) return false;
    mixer.writeStereo(0);
    if(mixer.advanceTo(48,out)!=1 || out[0].left || out[0].right) return false;
    mixer.writeStereo(0xff);mixer.writeStereo(1);
    if(mixer.advanceTo(64,out)!=1 || out[0].left || !near(out[0].right,.25)) return false;
    for(const uint32_t rate:{44100u,48000u,96000u}) {
        auto render=[&](bool split) {
            AudioMixer m(3579545,rate);m.writePsg(0x87);m.writePsg(0);m.writePsg(0x90);
            m.writePsg(0xe4);m.writePsg(0xf3);m.writeStereo(0x81);
            std::vector<AudioFrame> frames(uint64_t(100003)*rate/3579545+1);
            uint64_t time=0;size_t count=0;
            for(uint64_t target:{30001,100003}) {
                while(time<target) {
                    time=split?std::min(time+37,target):target;
                    count+=m.advanceTo(time,std::span(frames).subspan(count));
                }
                m.writeStereo(0x18);
            }
            frames.resize(count);return frames;
        };
        const auto whole=render(false),split=render(true);
        if(whole.size()!=uint64_t(100003)*rate/3579545 || split.size()!=whole.size()) return false;
        for(size_t i=0;i<whole.size();++i)
            if(!near(whole[i].left,split[i].left) || !near(whole[i].right,split[i].right)) return false;
    }
    return true;
}
bool system() {
    constexpr uint64_t clock=3579545;
    for(const auto machine:{Machine::Sms,Machine::GameGear}) for(const uint32_t rate:{44100u,48000u,96000u})
        for(const bool prefix:{false,true}) {
            std::vector<uint8_t> bytes(prefix?65536:8192,prefix?0xdd:0x76);
            const std::array<uint8_t,8> boot{0x3e,0x90,0xd3,0x7f,0x3e,0x10,0xd3,6};
            std::copy(boot.begin(),boot.end(),bytes.begin());
            if(prefix) bytes[0xbfff]=0;
            auto system=std::make_unique<EmulatorSystem>(std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),
                {prefix?MapperKind::Sega:MapperKind::RomOnly})),VideoStandard::Ntsc,rate,machine);
            uint64_t count=0,last=0;
            for(unsigned i=0;i<(prefix?5u:5000u);++i) {
                if(!std::holds_alternative<uint32_t>(system->step())) return false;
                for(const auto f:system->audioFrames()) {
                    const auto boundary=(++count*clock+rate-1)/rate;
                    const auto active=boundary-std::max(last,uint64_t{7});
                    const auto right=machine==Machine::Sms?active:last>=25?0:std::min(boundary,uint64_t{25})-std::max(last,uint64_t{7});
                    if(!near(f.left,.25*double(active)/double(boundary-last))
                        || !near(f.right,.25*double(right)/double(boundary-last))) return false;
                    last=boundary;
                }
                if(count!=system->tstates()*rate/clock) return false;
            }
            if(prefix && (system->tstates()!=196612 || count<2000)) return false;
        }
    return true;
}
}
int main(int argc,char** argv) {
    const auto name=argc==2?std::string_view(argv[1]):"";
    const bool pass=name=="masks"?masks():name=="phase"?phase():name=="system"&&system();
    if(!pass) std::cerr<<"GG audio test failed: "<<name<<'\n';
    return pass?0:1;
}
