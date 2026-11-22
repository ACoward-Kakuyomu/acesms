#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "Stage9TestRom.h"

#include <fstream>
#include <iostream>
#include <memory>

int main(int argc,char** argv) {
    using namespace smsgg;
    if(argc!=2) return 1;
    const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    for(unsigned profile=0;profile<8;++profile) {
        const bool withRam=profile>=4,gg=profile%4==2,fm=profile%4==3;
        const size_t ramBytes=withRam?8192:0;
        const auto standard=profile%4==1?VideoStandard::Pal:VideoStandard::Ntsc;
        const auto machine=gg?Machine::GameGear:Machine::Sms;
        const auto bytes=test::stage9CodemastersRom(gg,withRam);
        auto direct=RomLoader::fromBytes(bytes,{MapperKind::Codemasters,ramBytes}),detected=RomLoader::fromBytes(bytes,{MapperKind::Auto,ramBytes});
        if(!std::holds_alternative<RomImage>(direct) || !std::holds_alternative<RomImage>(detected)) return 1;
        auto& autoImage=std::get<RomImage>(detected);
        if(autoImage.options().mapper!=MapperKind::Codemasters || autoImage.metadata().mapperSelection!=MapperSelection::CodemastersHeader
            || !autoImage.metadata().header || autoImage.metadata().header->model!=(gg?RomModel::GameGear:RomModel::Sms)) return 1;
        auto a=std::make_unique<EmulatorSystem>(std::get<RomImage>(std::move(direct)),standard,48000,machine,fm);
        auto b=std::make_unique<EmulatorSystem>(std::move(autoImage),standard,48000,machine,fm);
        uint64_t samples=0;bool audible=false,visible=false;
        while(a->completedFrames()<85) {
            PadState pad{};pad.start=gg && a->completedFrames()>=20 && a->completedFrames()<30;
            pad.button1=!gg && a->completedFrames()>=20 && a->completedFrames()<30;
            const auto frame=a->completedFrames();
            const auto first=a->step(pad),second=b->step(pad);
            if(!std::holds_alternative<uint32_t>(first) || first!=second || a->tstates()!=b->tstates()) return 1;
            const auto pcm=a->audioFrames(),other=b->audioFrames();
            if(pcm.size()!=other.size()) return 1;
            for(size_t i=0;i<pcm.size();++i) {
                if(pcm[i].left!=other[i].left || pcm[i].right!=other[i].right) return 1;
                audible|=pcm[i].left!=0 || pcm[i].right!=0;
            }
            samples+=pcm.size();if(samples!=a->tstates()*48000/cpuClockHz(standard)) return 1;
            if(a->completedFrames()!=frame) {
                for(size_t i=0;i<a->framePixels().size();++i) {
                    const auto p=a->framePixels()[i],q=b->framePixels()[i];
                    if(p.r!=q.r || p.g!=q.g || p.b!=q.b || p.a!=q.a) return 1;
                    visible|=p.r || p.g || p.b;
                }
                if(a->completedFrames()==25) {
                    if(gg ? a->readMemory(0xc002)!=0x40 : a->readMemory(0xc002)!=0xef) return 1;
                }
            }
        }
        if(!visible || !audible || a->readMemory(0xc010)!=0xa3 || a->readMemory(0xc011)!=0xa2
            || b->readMemory(0xc010)!=0xa3 || b->readMemory(0xc011)!=0xa2) return 1;
        if(withRam) {
            if(a->readMemory(0xc012)!=0x5a || a->readMemory(0xc013)!=0xaa
                || b->readMemory(0xc012)!=0x5a || b->readMemory(0xc013)!=0xaa) return 1;
            auto absent=std::make_unique<EmulatorSystem>(std::get<RomImage>(RomLoader::fromBytes(bytes,{MapperKind::Codemasters})),standard,48000,machine,fm);
            while(absent->completedFrames()<5) if(!std::holds_alternative<uint32_t>(absent->step({}))) return 1;
            if(absent->readMemory(0xc012) || absent->readMemory(0xc013) || absent->readMemory(0xc005)) return 1;
        }
        const auto path=root/(withRam?(gg?"codemasters-ram.gg":"codemasters-ram.sms"):(gg?"codemasters.gg":"codemasters.sms"));
        {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out) return 1;}
        std::cout<<"profile="<<profile<<" RAM="<<ramBytes<<" frames="<<a->completedFrames()<<" samples="<<samples<<" bank signatures=A3,A2"<<(withRam?" RAM signatures=5A,AA":"")<<" PASS\n";
    }
    return 0;
}
