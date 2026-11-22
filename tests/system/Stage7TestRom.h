#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace smsgg::test {
// Original BIOS-free GG fixture. Tables are copied through the real OTIR/VDP path.
inline std::vector<uint8_t> stage7GameGearRom() {
    std::vector<uint8_t> rom(8192);
    size_t at=0;
    auto emit=[&](std::initializer_list<uint8_t> bytes) {for(auto b:bytes) rom[at++]=b;};
    auto out=[&](uint8_t port,uint8_t value) {emit({0x3e,value,0xd3,port});};
    auto address=[&](uint16_t a,uint8_t code) {out(0xbf,static_cast<uint8_t>(a));out(0xbf,static_cast<uint8_t>((a>>8)|(code<<6)));};
    auto reg=[&](uint8_t n,uint8_t v) {out(0xbf,v);out(0xbf,0x80|n);};
    auto jump=[&](uint8_t opcode=0xc3) {emit({opcode,0,0});return at-2;};
    auto patch=[&](size_t position,size_t target) {rom[position]=static_cast<uint8_t>(target);rom[position+1]=static_cast<uint8_t>(target>>8);};
    struct Data {size_t pointer;std::vector<uint8_t> bytes;};
    std::vector<Data> tables;
    auto transfer=[&](uint16_t dest,uint8_t code,std::vector<uint8_t> bytes) {
        address(dest,code);emit({0x21,0,0});const auto pointer=at-2;
        for(size_t pos=0;pos<bytes.size();) {
            const auto count=std::min<size_t>(256,bytes.size()-pos);
            emit({0x01,0xbe,static_cast<uint8_t>(count),0xed,0xb3});pos+=count;
        }
        tables.push_back({pointer,std::move(bytes)});
    };
    emit({0xc3,0,1});
    at=0x38;emit({0xf5,0xe5,0xdb,0xbf,0x21,0,0xc0,0x34,0xe1,0xf1,0xfb,0xed,0x4d});
    at=0x66;emit({0xf5,0xe5,0x21,1,0xc0,0x34,0xe1,0xf1,0xed,0x45});
    at=0x100;emit({0xf3,0x31,0xf0,0xdf,0xaf});
    for(uint8_t i=0;i<5;++i) emit({0x32,i,0xc0});
    emit({0x3e,0xff,0x32,5,0xc0});
    reg(0,6);reg(2,0xff);reg(3,0xff);reg(4,0xff);reg(5,0xff);reg(6,0xfb);
    std::vector<uint8_t> tiles;
    for(unsigned tile=0;tile<20;++tile) for(unsigned row=0;row<8;++row) for(unsigned plane=0;plane<4;++plane) {
        uint8_t bits=0;
        for(unsigned x=0;x<8;++x) {
            const auto color=tile<16?tile:((row+x)&1)?15:tile-15;
            if(color&(1<<plane)) bits|=static_cast<uint8_t>(0x80>>x);
        }
        tiles.push_back(bits);
    }
    transfer(0,1,std::move(tiles));
    std::vector<uint8_t> names;
    for(unsigned y=0;y<24;++y) for(unsigned x=0;x<32;++x) {
        unsigned tile=(x+3*y)%15+1,attribute=((x+y)&1)?8:0;
        if((x==6 || x==25) && (y==3 || y==20)) {tile=16+(x==25?1:0)+(y==20?2:0);attribute=0;}
        names.push_back(static_cast<uint8_t>(tile));names.push_back(static_cast<uint8_t>(attribute));
    }
    transfer(0x3800,1,std::move(names));
    std::vector<uint8_t> colors;
    for(unsigned i=0;i<32;++i) {
        const auto color=(i*73+0x123)&0xfff;
        colors.push_back(static_cast<uint8_t>(color));colors.push_back(static_cast<uint8_t>(color>>8));
    }
    transfer(0,3,std::move(colors));
    address(0x3f00,1);out(0xbe,0xd0);
    for(const uint8_t value:{0x8e,0x0f,0xac,0x1f,0xcf,0x07,0xe5,0x9f,0xbf,0xdf,0xff}) out(0x7f,value);
    out(6,0xff);reg(1,0xe0);emit({0xed,0x56,0xfb});
    const auto loop=at;
    emit({0x76,0xdb,0,0x32,2,0xc0,0xdb,0xdc,0x32,3,0xc0,
        0x3a,0,0xc0,0x07,0x07,0x07,0xe6,7,0x47,
        0x3a,2,0xc0,0xe6,0x80,0x20,2,0x06,8,
        0x78,0x32,4,0xc0,0x3a,5,0xc0,0xb8});
    const auto unchanged=jump(0xca);
    emit({0x78,0x32,5,0xc0});
    for(const uint8_t value:{0x9f,0xbf,0xdf,0xff}) out(0x7f,value);
    std::array<size_t,9> branches;
    for(uint8_t stage=0;stage<9;++stage) {emit({0x78,0xfe,stage});branches[stage]=jump(0xca);}
    std::array<size_t,9> done;
    constexpr uint8_t masks[]{0x10,2,0x88,0,0x88,0x21,0xff,0xff,0x40};
    for(unsigned stage=0;stage<9;++stage) {
        patch(branches[stage],at);
        if(stage==0 || stage==5 || stage==6) out(0x7f,0x90);
        if(stage==1 || stage==5 || stage==6) out(0x7f,0xb0);
        if(stage==6 || stage==8) out(0x7f,0xd0);
        if((stage>=2 && stage<=4) || stage==6) out(0x7f,0xf0);
        out(6,masks[stage]);done[stage]=jump();
    }
    const auto draw=at;
    patch(unchanged,draw);for(auto position:done) patch(position,draw);
    emit({0x3a,2,0xc0,0xe6,0x80,0x06,0x6c,0x20,2,0x06,0xce});
    address(2,3);emit({0x78,0xd3,0xbe,0x3a,2,0xc0,0xe6,0x80,0x3e,1,0x20,2,0x3e,0x0a,0xd3,0xbe});
    const auto again=jump();patch(again,loop);
    for(auto& data:tables) {patch(data.pointer,at);for(auto b:data.bytes) rom[at++]=b;}
    constexpr std::array<uint8_t,8> signature{'T','M','R',' ','S','E','G','A'};
    std::copy(signature.begin(),signature.end(),rom.begin()+0x1ff0);rom[0x1fff]=0x6a;
    return rom;
}
}
