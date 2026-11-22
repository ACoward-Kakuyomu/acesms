#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "Stage8TestRom.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

int main(int argc,char** argv) {
    using namespace smsgg;
    if(argc!=2) return 1;
    const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    const auto bytes=test::stage8FmRom();
    {std::ofstream out(root/"fm.sms",std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out) return 1;}
    for(unsigned profile=0;profile<4;++profile) {
        const bool fm=profile!=0;
        const auto method=fm?static_cast<FmInterpolation>(profile-1):FmInterpolation::Sinc;
        auto system=std::make_unique<EmulatorSystem>(std::get<RomImage>(RomLoader::fromBytes(bytes,{MapperKind::RomOnly})),
            VideoStandard::Ntsc,48000,Machine::Sms,fm,method);
        std::array<size_t,8> samples{},nonzero{};
        std::array<bool,8> seen{};
        constexpr uint8_t colors[]{0x03,0x0c,0x00,0x0f,0x30,0x3c,0x33,0x3f};
        uint64_t total=0;
        while(system->completedFrames()<258) {
            const auto before=system->completedFrames();
            if(!std::holds_alternative<uint32_t>(system->step())) return 1;
            const auto stage=system->readMemory(0xc002),frame=system->readMemory(0xc000);
            if(stage>7) {std::cerr<<"invalid stage="<<unsigned(stage)<<" pc="<<system->cpu().registers().pc<<'\n';return 1;}
            for(const auto pcm:system->audioFrames()) {
                if(!std::isfinite(pcm.left) || pcm.left!=pcm.right || std::abs(pcm.left)>1) return 1;
                ++total;
                if((frame&31)>=8 && (frame&31)<=25) {++samples[stage];nonzero[stage]+=std::abs(pcm.left)>1e-5f;}
            }
            if(total!=system->tstates()*48000/3579545) return 1;
            if(system->completedFrames()!=before && (frame&31)==20) {
                const uint8_t color=fm?colors[stage]:3;
                const Rgba8 expected{static_cast<uint8_t>((color&3)*85),static_cast<uint8_t>(((color>>2)&3)*85),
                    static_cast<uint8_t>(((color>>4)&3)*85),255};
                for(const auto pixel:system->framePixels())
                    if(pixel.r!=expected.r || pixel.g!=expected.g || pixel.b!=expected.b || pixel.a!=255) {
                        std::cerr<<"pixel fm="<<fm<<" stage="<<unsigned(stage)<<" frame="<<unsigned(frame)
                            <<" got="<<unsigned(pixel.r)<<','<<unsigned(pixel.g)<<','<<unsigned(pixel.b)
                            <<" expected="<<unsigned(expected.r)<<','<<unsigned(expected.g)<<','<<unsigned(expected.b)<<'\n';return 1;
                    }
                seen[stage]=true;
            }
        }
        if(system->readMemory(0xc001)!=(fm?1:0)) return 1;
        for(unsigned stage=0;stage<8;++stage) {
            std::cout<<"fm="<<fm<<" method="<<static_cast<unsigned>(method)<<" stage="<<stage<<" samples="<<samples[stage]<<" nonzero="<<nonzero[stage]<<'\n';
            if(!seen[stage] || samples[stage]<10000) return 1;
            if(fm && stage==2) {if(nonzero[stage]) return 1;}
            else if(stage!=6 && !nonzero[stage]) return 1;
        }
    }
    return 0;
}
