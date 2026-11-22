#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace smsgg::test {
// Original test program: no BIOS, music, or third-party ROM content.
inline std::vector<uint8_t> stage8FmRom() {
    std::vector<uint8_t> rom(8192);
    size_t at=0;
    auto emit=[&](std::initializer_list<uint8_t> bytes){for(auto b:bytes) rom[at++]=b;};
    auto out=[&](uint8_t port,uint8_t value){emit({0x3e,value,0xd3,port});};
    auto fm=[&](uint8_t reg,uint8_t value){out(0xf0,reg);out(0xf1,value);};
    auto vdp=[&](uint8_t reg,uint8_t value){out(0xbf,value);out(0xbf,0x80|reg);};
    auto jump=[&](uint8_t op=0xc3){emit({op,0,0});return at-2;};
    auto patch=[&](size_t offset,size_t target){rom[offset]=static_cast<uint8_t>(target);rom[offset+1]=static_cast<uint8_t>(target>>8);};
    emit({0xc3,0x00,0x01});
    at=0x38;emit({0xf5,0xe5,0xdb,0xbf,0x21,0x00,0xc0,0x34,0xe1,0xf1,0xfb,0xed,0x4d});
    at=0x66;emit({0xed,0x45});
    at=0x100;emit({0xf3,0x31,0xf8,0xdf,0xaf,0x32,0x00,0xc0,0x0e,0x01});
    out(0x3e,4);
    for(uint8_t i=0;i<8;++i) {
        out(0xf2,i&1);emit({0xdb,0xf2,0xe6,7,0xfe,static_cast<uint8_t>(i&1),0x28,2,0x0e,0});
    }
    emit({0x79,0x32,0x01,0xc0});out(0x3e,0);
    for(const uint8_t value:{0x9f,0xbf,0xdf,0xff}) out(0x7f,value);
    out(0x7f,0x8e);out(0x7f,0x0f);out(0x7f,0x90);
    vdp(0,4);vdp(7,1);vdp(1,0x20); // Display off: a uniform, verifiable backdrop.
    emit({0x3e,0xff,0x32,0x03,0xc0,0xed,0x56,0xfb});
    const auto loop=at;
    emit({0x76,0x3a,0x00,0xc0,0x0f,0x0f,0x0f,0x0f,0x0f,0xe6,7,0x47,0x32,0x02,0xc0,
        0x3a,0x03,0xc0,0xb8});
    const auto unchanged=jump(0xca);patch(unchanged,loop);
    emit({0x78,0x32,0x03,0xc0});
    fm(0x20,0x09);fm(0x0e,0);
    std::vector<size_t> branches,done;
    for(uint8_t stage=0;stage<8;++stage){emit({0x78,0xfe,stage});branches.push_back(jump(0xca));}
    for(uint8_t stage=0;stage<8;++stage) {
        patch(branches[stage],at);
        const uint8_t mode=stage<4?stage:stage==7?0:1;
        out(0xf2,mode);
        if(stage==4) {
            const uint8_t patchBytes[]{0x21,0x21,0,0,0xf2,0xf2,4,4};
            for(uint8_t r=0;r<8;++r) fm(r,patchBytes[r]);
        }
        if(stage==5) {
            for(uint8_t ch=6;ch<9;++ch){fm(0x10+ch,0x80);fm(0x20+ch,0x09);fm(0x30+ch,0);}
            fm(0x0e,0x3f);
        } else if(stage!=6) {fm(0x30,stage==4?0:0x10);fm(0x10,0x80);fm(0x20,0x19);}
        done.push_back(jump());
    }
    const auto draw=at;
    for(auto offset:done) patch(offset,draw);
    // Color table indexed by the current stage; absent FM always shows red.
    emit({0x21,0,0});const auto table=at-2;
    emit({0x78,0x5f,0x16,0,0x19,0x46,0x3a,0x01,0xc0,0xb7,0x20,2,0x06,3});
    out(0xbf,17);out(0xbf,0xc0);emit({0x78,0xd3,0xbe});
    const auto again=jump();patch(again,loop);
    patch(table,at);emit({0x03,0x0c,0x00,0x0f,0x30,0x3c,0x33,0x3f});
    return rom;
}
} // namespace smsgg::test
