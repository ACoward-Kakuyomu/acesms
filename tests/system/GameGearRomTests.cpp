#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "Stage7TestRom.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

using namespace smsgg;
namespace {
Rgba8 expected(unsigned x,unsigned y,bool start) {
    const unsigned tx=x/8,ty=y/8;
    unsigned index=(tx+3*ty)%15+1 + (((tx+ty)&1)?16:0);
    if((tx==6 || tx==25) && (ty==3 || ty==20))
        index=((x+y)&1)?15:1+(tx==25?1:0)+(ty==20?2:0);
    const auto raw=start && index==1?0xace:(index*73+0x123)&0xfff;
    return {uint8_t((raw&15)*17),uint8_t(((raw>>4)&15)*17),uint8_t(((raw>>8)&15)*17),255};
}
bool pixels(const EmulatorSystem& system,bool start) {
    for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x) {
        const auto a=system.framePixels()[y*256+x],b=expected(x,y,start);
        if(a.r!=b.r || a.g!=b.g || a.b!=b.b || a.a!=b.a) {
            std::cerr<<"GG ROM pixel "<<x<<','<<y<<" frame="<<system.completedFrames()<<'\n';return false;
        }
    }
    return true;
}
double frequency(const std::vector<AudioFrame>& frames,bool right) {
    unsigned crossings=0;
    for(size_t i=1;i<frames.size();++i) {
        const float a=right?frames[i-1].right:frames[i-1].left,b=right?frames[i].right:frames[i].left;
        crossings+=(a<0 && b>=0);
    }
    return double(crossings)*48000/double(frames.size());
}
bool run(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto rom=test::stage7GameGearRom();
    {std::ofstream file(root/"gamegear.gg",std::ios::binary);file.write(reinterpret_cast<const char*>(rom.data()),rom.size());if(!file) return false;}
    auto system=std::make_unique<EmulatorSystem>(std::get<RomImage>(RomLoader::fromBytes(rom,{MapperKind::RomOnly})),
        VideoStandard::Ntsc,48000,Machine::GameGear);
    uint64_t count=0;
    auto advanceFrame=[&](PadState pad) {
        std::vector<AudioFrame> audio;
        const auto before=system->completedFrames();
        do {
            if(!std::holds_alternative<uint32_t>(system->step(pad,true))) throw std::runtime_error("GG ROM CPU fault");
            count+=system->audioFrames().size();
            audio.insert(audio.end(),system->audioFrames().begin(),system->audioFrames().end());
            if(count!=system->tstates()*48000/3579545) throw std::runtime_error("GG ROM sample count");
        } while(system->completedFrames()==before);
        return audio;
    };
    std::array<std::vector<AudioFrame>,9> stages;
    unsigned previous=255,stable=0;
    for(unsigned frame=0;frame<264;++frame) {
        auto audio=advanceFrame({});
        const unsigned stage=system->readMemory(0xc004);
        stable=stage==previous?stable+1:0;previous=stage;
        if(stage>=8 || system->readMemory(0xc001)) return false;
        if(stable>2) stages[stage].insert(stages[stage].end(),audio.begin(),audio.end());
        if(frame>=3 && !pixels(*system,false)) return false;
    }
    for(unsigned i=0;i<8;++i) {
        if(stages[i].size()<10000) {std::cerr<<"Missing GG sound stage "<<i<<'\n';return false;}
        bool signal=false;
        for(const auto f:stages[i]) {
            if(!std::isfinite(f.left) || !std::isfinite(f.right) || std::abs(f.left)>1.00001 || std::abs(f.right)>1.00001) return false;
            if((i==0 && f.right) || (i==1 && f.left) || ((i==3 || i==7) && (f.left || f.right))) return false;
            if((i==2 || i==4 || i==6) && f.left!=f.right) return false;
            signal=signal || f.left!=0 || f.right!=0;
        }
        if(i!=3 && i!=7 && !signal) return false;
    }
    if(std::abs(frequency(stages[0],false)-440.4)>8 || std::abs(frequency(stages[1],true)-220.2)>8
        || std::abs(frequency(stages[5],false)-220.2)>8 || std::abs(frequency(stages[5],true)-440.4)>8) return false;
    for(unsigned frame=0;frame<8;++frame) {
        auto audio=advanceFrame({.button1=true,.start=true});
        if(frame>2) stages[8].insert(stages[8].end(),audio.begin(),audio.end());
    }
    if(!pixels(*system,true) || system->readMemory(0xc001) || system->readMemory(0xc002)!=0x40
        || system->readMemory(0xc003)!=0xef || system->readMemory(0xc004)!=8) return false;
    if(std::abs(frequency(stages[8],false)-880.8)>15) return false;
    for(const auto f:stages[8]) if(f.right) return false;
    for(unsigned frame=0;frame<4;++frame) advanceFrame({});
    if(!pixels(*system,false) || system->readMemory(0xc002)!=0xc0 || system->readMemory(0xc003)!=0xff || system->readMemory(0xc001)) return false;
    const auto area=system->displayArea();
    std::ofstream image(root/"gamegear.ppm",std::ios::binary);image<<"P6\n160 144\n255\n";
    for(unsigned y=area.y;y<area.y+area.height;++y) for(unsigned x=area.x;x<area.x+area.width;++x) {
        const auto p=system->framePixels()[y*256+x];const char rgb[]{char(p.r),char(p.g),char(p.b)};image.write(rgb,3);
    }
    return bool(image);
}
}
int main(int argc,char** argv) {
    try {return argc==2 && run(argv[1])?0:1;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;}
}
