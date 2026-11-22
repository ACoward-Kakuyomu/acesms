#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "Stage6TestRom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

namespace {
void writeWav(const std::filesystem::path& path,const std::vector<smsgg::AudioFrame>& pcm) {
    std::ofstream out(path,std::ios::binary);
    auto number=[&](uint32_t n,unsigned bytes) {for(unsigned i=0;i<bytes;++i) out.put(static_cast<char>(n>>(8*i)));};
    out.write("RIFF",4); number(36+static_cast<uint32_t>(pcm.size()*4),4);out.write("WAVEfmt ",8);
    number(16,4);number(1,2);number(2,2);number(48000,4);number(192000,4);number(4,2);number(16,2);
    out.write("data",4);number(static_cast<uint32_t>(pcm.size()*4),4);
    for(const auto f:pcm) for(const auto sample:{f.left,f.right})
        number(static_cast<uint16_t>(std::clamp(std::lround(sample*32768),-32768l,32767l)),2);
    if(!out) throw std::runtime_error("WAV write failed");
}
bool run(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto bytes=smsgg::test::stage6AudioRom();
    { std::ofstream out(root/"sound.sms",std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); if(!out) return false; }
    for(const auto standard:{smsgg::VideoStandard::Ntsc,smsgg::VideoStandard::Pal}) {
        auto system=std::make_unique<smsgg::EmulatorSystem>(std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(bytes,{smsgg::MapperKind::RomOnly})),standard,48000);
        std::array<std::vector<float>,4> sections;
        std::vector<smsgg::AudioFrame> pcm;
        uint64_t total=0;
        while(system->completedFrames()<260) {
            if(!std::holds_alternative<uint32_t>(system->step())) return false;
            const auto stage=system->readMemory(0xc004);
            if(stage>3) return false;
            const auto irq=system->readMemory(0xc000);
            for(const auto f:system->audioFrames()) {
                if(!std::isfinite(f.left) || f.left!=f.right || std::abs(f.left)>.251) return false;
                ++total; pcm.push_back(f);
                if((irq&63)>=5 && (irq&63)<60) sections[stage].push_back(f.left);
            }
            if(total!=system->tstates()*48000/smsgg::cpuClockHz(standard)) return false;
            if(system->completedFrames()==5 && total>2000
                && std::none_of(pcm.begin(),pcm.end(),[](const auto& f){return std::abs(f.left)>.1f;})) return false;
        }
        for(unsigned stage=0;stage<4;++stage) {
            if(sections[stage].size()<10000) return false;
            if(stage==3) {for(auto x:sections[stage]) if(x!=0) return false;}
            else {
                const auto [low,high]=std::minmax_element(sections[stage].begin(),sections[stage].end());
                if(*low>-.2f || *high<.2f) return false;
                if(stage<2) {
                    unsigned edges=0; for(size_t i=1;i<sections[stage].size();++i)
                        if(sections[stage][i-1]<=0 && sections[stage][i]>0) ++edges;
                    const double measured=double(edges)*48000/double(sections[stage].size());
                    const double expected=double(smsgg::cpuClockHz(standard))/(32*(stage?508:254));
                    if(std::abs(measured-expected)>3) return false;
                }
            }
        }
        auto until=[&](uint64_t frames,smsgg::PadState pad={},bool pause=false) {
            const auto end=system->completedFrames()+frames;
            do {system->step(pad,pause);pause=false;} while(system->completedFrames()<end);
        };
        until(3,{.button1=true});
        if(system->readMemory(0xc004)!=1 || system->readMemory(0xc002)!=0xef) return false;
        auto checkFrequency=[&](smsgg::PadState pad,double expected) {
            std::vector<float> samples;
            while(samples.size()<12000) {
                system->step(pad);
                for(auto f:system->audioFrames()) samples.push_back(f.left);
            }
            unsigned edges=0;
            for(size_t i=1;i<samples.size();++i) if(samples[i-1]<=0 && samples[i]>0) ++edges;
            const auto [low,high]=std::minmax_element(samples.begin(),samples.end());
            return *low<-.2f && *high>.2f && std::abs(double(edges)*48000/double(samples.size())-expected)<8;
        };
        until(3,{.button2=true}); if(system->readMemory(0xc004)!=4) return false;
        if(!checkFrequency({.button2=true},double(smsgg::cpuClockHz(standard))/(32*127))) return false;
        until(3,{.up=true}); if(system->readMemory(0xc004)!=5) return false;
        if(!checkFrequency({.up=true},double(smsgg::cpuClockHz(standard))/(2048*16))) return false;
        until(3,{.down=true}); if(system->readMemory(0xc004)!=6) return false;
        std::array<bool,16> volumes{};
        for(unsigned i=0;i<20;++i) {
            until(1,{.down=true});
            // Main-loop update follows vblank IRQ; wait into the next visible area.
            for(unsigned j=0;j<600;++j) system->step({.down=true});
            const auto attenuation=system->readMemory(0xc000)&15;
            for(unsigned j=0;j<100;++j) {
                system->step({.down=true});
                for(auto f:system->audioFrames()) {
                    const double expected=attenuation==15?0:.25*std::pow(10.0,-double(attenuation)/10);
                    if(std::abs(f.left-expected)>1e-6) return false;
                    volumes[attenuation]=true;
                }
            }
        }
        if(std::find(volumes.begin(),volumes.end(),false)!=volumes.end()) return false;
        until(3,{},true);
        if(system->readMemory(0xc004)!=3 || system->readMemory(0xc001)!=1) return false;
        for(unsigned i=0;i<192*256;++i) {
            const auto p=system->framePixels()[i]; const bool colored=((i%256)+(i/256))%2==0;
            if(p.r!=(colored?255:0) || p.g!=(colored?255:0) || p.b!=255 || p.a!=255) return false;
        }
        if(standard==smsgg::VideoStandard::Ntsc) writeWav(root/"sound-ntsc-48000.wav",pcm);
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc!=2 || !run(argv[1])) {std::cerr<<"Audio ROM test failed\n";return 1;}
    return 0;
}
