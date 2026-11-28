#include "VdpTestSupport.h"

#include <iostream>
#include <memory>
#include <string_view>

namespace {
using namespace smsgg;
using test::command;
using test::reg;
bool pixel(Rgba8 a,Rgba8 b) { return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a; }
bool same(const VdpState& a,const VdpState& b) {
    if(a.vram!=b.vram || a.address!=b.address || a.code!=b.code || a.buffer!=b.buffer
        || a.secondByte!=b.secondByte || a.line!=b.line || a.cycle!=b.cycle || a.frames!=b.frames
        || a.hLatch!=b.hLatch || a.registers!=b.registers || a.framePending!=b.framePending
        || a.linePending!=b.linePending || a.lineCounter!=b.lineCounter || a.cramLatch!=b.cramLatch
        || a.drawing!=b.drawing || a.scrollY!=b.scrollY || a.spriteStatus!=b.spriteStatus
        || a.cramChangeCount!=b.cramChangeCount) return false;
    for(unsigned i=0;i<a.cramChangeCount;++i) {
        const auto& x=a.cramChanges[i];const auto& y=b.cramChanges[i];
        if(x.dot!=y.dot || x.index!=y.index || !pixel(x.previous,y.previous)) return false;
    }
    for(size_t i=0;i<a.palette.size();++i) if(!pixel(a.palette[i],b.palette[i])) return false;
    for(size_t frame=0;frame<2;++frame) for(size_t i=0;i<a.images[frame].size();++i)
        if(!pixel(a.images[frame][i],b.images[frame][i])) return false;
    return true;
}
bool equal(Vdp& a,Vdp& b) {
    auto first=std::make_unique<VdpState>();auto second=std::make_unique<VdpState>();
    a.captureState(*first);b.captureState(*second);return same(*first,*second);
}
void palette(Vdp& v,Machine machine,unsigned index,unsigned color) {
    command(v,static_cast<uint16_t>(machine==Machine::GameGear?index*2:index),3);
    v.writeData(static_cast<uint8_t>(color));
    if(machine==Machine::GameGear) v.writeData(static_cast<uint8_t>(color>>8));
}
bool ports() {
    for(unsigned profile=0;profile<3;++profile) for(unsigned phase=0;phase<3;++phase) {
        const auto standard=profile==1?VideoStandard::Pal:VideoStandard::Ntsc;
        const auto machine=profile==2?Machine::GameGear:Machine::Sms;
        auto original=std::make_unique<Vdp>(standard,machine),restored=std::make_unique<Vdp>(standard,machine);
        command(*original,0,1);
        for(unsigned i=0;i<16384;++i) original->writeData(static_cast<uint8_t>(i*11+i/256));
        for(unsigned i=0;i<32;++i) palette(*original,machine,i,i*117+0x123);
        if(phase==0) {command(*original,0x1234,0);original->writeControl(0xab);}
        else if(phase==1) command(*original,0x3fff,0);
        else {command(*original,0x20,3);original->writeData(0xba);}
        auto saved=std::make_unique<VdpState>();original->captureState(*saved);
        if(phase==0 && (!saved->secondByte || saved->address!=0x12ab)) return false;
        if(phase==1 && (saved->address || saved->buffer!=static_cast<uint8_t>(0x3fff*11+63))) return false;
        if(phase==2 && machine==Machine::GameGear && saved->cramLatch!=0xba) return false;
        command(*restored,0x20,3);restored->writeData(0x12);restored->writeControl(0x77);
        restored->restoreState(*saved);
        if(!equal(*original,*restored)) return false;
        if(phase==0) {
            original->writeControl(0x89);restored->writeControl(0x89);
            auto after=std::make_unique<VdpState>();restored->captureState(*after);
            if(after->registers[9]!=0xab || after->secondByte) return false;
        } else if(phase==1) {
            for(unsigned i=0;i<3;++i) {
                const auto expected=static_cast<uint8_t>((i?i-1:0x3fff)*11+(i?0:63));
                if(original->readData()!=expected || restored->readData()!=expected) return false;
            }
        } else {
            original->writeData(0xfc);restored->writeData(0xfc);
            if(machine==Machine::GameGear) {
                auto after=std::make_unique<VdpState>();restored->captureState(*after);
                if(!pixel(after->palette[16],{0xaa,0xbb,0xcc,255})) return false;
            }
        }
        if(!equal(*original,*restored)) return false;
        command(*original,0,1);original->writeData(0x91);
        if(saved->vram[0]!=0) return false;
        restored->restoreState(*saved);
        auto again=std::make_unique<VdpState>();restored->captureState(*again);
        if(!same(*saved,*again)) return false;
    }
    return true;
}
bool timing() {
    for(unsigned profile=0;profile<3;++profile) {
        const auto standard=profile==1?VideoStandard::Pal:VideoStandard::Ntsc;
        const auto machine=profile==2?Machine::GameGear:Machine::Sms;
        const uint64_t period=(profile==1?313:262)*228;
        auto original=std::make_unique<Vdp>(standard,machine),restored=std::make_unique<Vdp>(standard,machine);
        reg(*original,0,0x14);reg(*original,1,0x60);reg(*original,2,0x0e);reg(*original,5,0x7e);reg(*original,10,1);
        command(*original,0,1);
        for(unsigned row=0;row<8;++row) for(unsigned plane=0;plane<4;++plane) original->writeData(plane?0:0xff);
        command(*original,0x3f00,1);for(unsigned i=0;i<9;++i) original->writeData(0);original->writeData(0xd0);
        original->advanceTo(period);original->readStatus();
        const uint64_t at=period+4*228+97;original->advanceTo(at);original->latchHCounter();
        const uint8_t hLatch=machine==Machine::Sms?73:72;
        if(!original->interruptPending() || original->readVCounter()!=4 || original->readHCounter()!=hLatch) return false;
        auto saved=std::make_unique<VdpState>();original->captureState(*saved);
        if(saved->spriteStatus!=0x60 || saved->framePending || !saved->linePending || !original->interruptPending()) return false;
        restored->advanceTo(period*4+31);restored->readStatus();restored->restoreState(*saved);
        if(!equal(*original,*restored)) return false;
        for(const uint64_t delta:{0ull,1ull,130ull,131ull,228ull,456ull,193ull*228,period}) {
            original->advanceTo(at+delta);restored->advanceTo(at+delta);
            if(original->readVCounter()!=restored->readVCounter() || restored->readHCounter()!=hLatch
                || original->interruptPending()!=restored->interruptPending() || !equal(*original,*restored)) return false;
            if(delta==193*228) {
                const auto status=original->readStatus();if(!(status&0x80) || restored->readStatus()!=status) return false;
            }
        }
        if (machine == Machine::Sms) {
            reg(*original, 10, 0);
            const auto edge = period * 3 - 15; // IRQ for the next frame's first line.
            original->advanceTo(edge - 1);
            original->readStatus();
            original->captureState(*saved);
            restored->restoreState(*saved);
            original->advanceTo(edge);
            restored->advanceTo(edge);
            if (!restored->interruptPending() || !equal(*original, *restored)) return false;
            original->readStatus();
            original->captureState(*saved);
            restored->restoreState(*saved);
            original->advanceTo(period * 3);
            restored->advanceTo(period * 3);
            if (restored->interruptPending() || !equal(*original, *restored)) return false;
        }
    }
    return true;
}
bool frames() {
    constexpr Rgba8 red{255,0,0,255},green{0,255,0,255},blue{0,0,255,255};
    for(unsigned profile=0;profile<3;++profile) {
        const auto standard=profile==1?VideoStandard::Pal:VideoStandard::Ntsc;
        const auto machine=profile==2?Machine::GameGear:Machine::Sms;
        const uint64_t period=(profile==1?313:262)*228;
        auto original=std::make_unique<Vdp>(standard,machine),restored=std::make_unique<Vdp>(standard,machine);
        reg(*original,9,17);palette(*original,machine,16,machine==Machine::GameGear?0xf:3);
        original->advanceTo(period);
        palette(*original,machine,16,machine==Machine::GameGear?0xf0:0xc);reg(*original,9,69);
        original->advanceTo(period+90*228+79);
        auto saved=std::make_unique<VdpState>();original->captureState(*saved);
        if(saved->scrollY!=17 || saved->registers[9]!=69 || saved->line!=90 || saved->cycle%228!=79
            || !pixel(saved->images[saved->drawing][89*256],green) || !pixel(original->framePixels()[0],red)) return false;
        palette(*restored,machine,16,machine==Machine::GameGear?0xf00:0x30);restored->advanceTo(period*4);
        restored->restoreState(*saved);
        if(!pixel(restored->framePixels()[0],red) || !equal(*original,*restored)) return false;
        palette(*original,machine,16,machine==Machine::GameGear?0xf00:0x30);
        palette(*restored,machine,16,machine==Machine::GameGear?0xf00:0x30);
        original->advanceTo(period*2);restored->advanceTo(period*2);
        if(!equal(*original,*restored)) return false;
        for(unsigned y=0;y<192;++y) for(unsigned x=0;x<256;++x) {
            const bool beforeWrite = y < 90 || (machine == Machine::Sms && y == 90 && x < 119);
            if(!pixel(restored->framePixels()[y*256+x],beforeWrite?green:blue)) return false;
        }
        original->advanceTo(period*3);restored->advanceTo(period*3);
        if(!equal(*original,*restored)) return false;
        for(auto p:restored->framePixels()) if(!pixel(p,blue)) return false;
        if(!pixel(saved->images[saved->drawing^1][0],red) || !pixel(saved->images[saved->drawing][0],green)) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2) {
        const std::string_view name(argv[1]);
        if(name=="ports"?ports():name=="timing"?timing():name=="frames" && frames()) return 0;
        std::cerr<<"VDP state "<<name<<" failed\n";
    }
    return 1;
}
