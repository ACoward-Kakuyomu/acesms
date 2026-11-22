#include "core/Machine.h"
#include "VdpTestSupport.h"

#include <iostream>
#include <memory>
#include <string_view>

using namespace smsgg;
using test::command;
using test::reg;
namespace {
bool same(Rgba8 a,Rgba8 b) {return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a;}
Rgba8 color(unsigned raw) {return {uint8_t((raw%16)*17),uint8_t((raw/16%16)*17),uint8_t((raw/256%16)*17),255};}
void palette(Vdp& v,unsigned index,unsigned raw) {
    command(v,static_cast<uint16_t>(index*2),3);v.writeData(static_cast<uint8_t>(raw));
    v.writeData(static_cast<uint8_t>(0xf0|(raw>>8)));
}
bool frame(Vdp& v,uint64_t& time,Rgba8 expected) {
    v.advanceTo(time+=59736);
    return same(v.framePixels()[24*256+48],expected) && same(v.framePixels()[167*256+207],expected);
}
bool colors() {
    auto v=std::make_unique<Vdp>(VideoStandard::Ntsc,Machine::GameGear);
    uint64_t time=0;
    for(unsigned raw=0;raw<4096;++raw) {
        const auto index=raw%16;
        reg(*v,7,static_cast<uint8_t>(index));palette(*v,16+index,raw);
        if(!frame(*v,time,color(raw))) {std::cerr<<"GG color="<<raw<<'\n';return false;}
    }
    reg(*v,0,6);reg(*v,1,0x40);reg(*v,2,0x0e);reg(*v,5,0x7e);
    command(*v,0x3f00,1);v->writeData(0xd0);
    for(unsigned index=0;index<32;++index) {
        const unsigned raw=(index*127+0x123)&0xfff;palette(*v,index,raw);
        command(*v,32,1);
        for(unsigned row=0;row<8;++row) for(unsigned plane=0;plane<4;++plane)
            v->writeData((index&(1<<plane))?0xff:0);
        command(*v,0x3800,1);
        for(unsigned entry=0;entry<768;++entry) {v->writeData(1);v->writeData(index>=16?8:0);}
        if(!frame(*v,time,color(raw))) return false;
    }
    return true;
}
bool protocol() {
    auto v=std::make_unique<Vdp>(VideoStandard::Ntsc,Machine::GameGear);
    uint64_t time=0;
    // Odd-only write uses the initial zero R/G latch. High nibble is ignored by color, not the read buffer.
    command(*v,33,3);v->writeData(0xf3);
    if(v->readData()!=0xf3 || !frame(*v,time,color(0x300))) return false;
    command(*v,0x30,3);v->writeData(0xba);
    if(!frame(*v,time,color(0x300))) return false;
    v->readStatus();command(*v,0x100,1);v->writeData(0x65); // VRAM must not replace R/G latch.
    command(*v,0x100,0);if(v->readData()!=0x65) return false;
    command(*v,33,3);v->writeData(0xfc);
    if(!frame(*v,time,color(0xcba))) return false;
    command(*v,0x20,3);v->writeData(0x12);v->writeData(0x04);
    if(!frame(*v,time,color(0x412))) return false;
    command(*v,0x20,3);v->writeData(0x56);
    command(*v,0x20,3);v->writeData(0x78); // Consecutive even writes retain only latest data.
    command(*v,0x3fff,3);v->writeData(0xfa); // index 31, then wrap to 0000.
    v->writeData(0x9b);v->writeData(0x0d); // index 0, latched after wrap.
    reg(*v,7,15);if(!frame(*v,time,color(0xa78))) return false;
    reg(*v,0,6);reg(*v,1,0x40);reg(*v,2,0x0e);reg(*v,5,0x7e);
    command(*v,0x3f00,1);v->writeData(0xd0);
    if(!frame(*v,time,color(0xd9b))) return false; // Empty tile/name table selects palette entry 0.
    reg(*v,1,0);
    // A CRAM address mirror at 60/61 updates entry 16.
    reg(*v,7,0);command(*v,0x60,3);v->writeData(0x46);v->writeData(0x08);
    if(!frame(*v,time,color(0x846))) return false;
    // Cross 3F -> 40: high at 3F commits index 31; even 40 only changes the latch.
    command(*v,0x3f,3);v->writeData(0x0e);v->writeData(0xab);
    reg(*v,7,15);if(!frame(*v,time,color(0xe46))) return false;
    command(*v,0x21,3);v->writeData(0x0f);reg(*v,7,0);
    return frame(*v,time,color(0xfab));
}
bool offscreen() {
    auto v=std::make_unique<Vdp>(VideoStandard::Ntsc,Machine::GameGear);
    reg(*v,0,0x16);reg(*v,1,0x60);reg(*v,2,0x0e);reg(*v,5,0x7e);reg(*v,10,0);
    command(*v,32,1);
    for(unsigned row=0;row<8;++row) for(unsigned plane=0;plane<4;++plane) v->writeData(plane?0:0xff);
    command(*v,0x3f00,1);v->writeData(0xff);v->writeData(0xff);v->writeData(0xd0);
    command(*v,0x3f80,1);v->writeData(0);v->writeData(1);v->writeData(0);v->writeData(1);
    v->advanceTo(228); // Both sprites overlap at (0,0), outside the LCD crop.
    if(!(v->readStatus()&0x20) || v->readVCounter()!=1) return false;
    v->advanceTo(59736);
    if(v->completedFrames()!=1 || v->readVCounter()!=0 || !v->interruptPending() || !(v->readStatus()&0x80)) return false;
    if(v->interruptPending()) return false;
    v->advanceTo(59736+228);
    return v->completedFrames()==1 && v->readVCounter()==1 && v->interruptPending() && !(v->readStatus()&0x80);
}
}
int main(int argc,char** argv) {
    const auto name=argc==2?std::string_view(argv[1]):"";
    const bool pass=name=="colors"?colors():name=="protocol"?protocol():name=="offscreen"&&offscreen();
    if(!pass) std::cerr<<"GG VDP test failed: "<<name<<'\n';
    return pass?0:1;
}
