#include "core/audio/Emu2413Native.h"

#include <emu2413.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using smsgg::Emu2413Native;
template<class Chip> void write(Chip& chip,uint8_t address,uint8_t value) {
    chip.writeIo(0,address);chip.writeIo(1,value);
}
template<class Chip> void configure(Chip& chip,unsigned mode) {
    if(mode==1) {
        constexpr std::array<uint8_t,8> patch{0x21,0x21,0x00,0x00,0xf2,0xf2,0x04,0x04};
        for(uint8_t i=0;i<patch.size();++i) write(chip,i,patch[i]);
    }
    if(mode==2) {
        for(uint8_t ch=6;ch<9;++ch) {write(chip,0x10+ch,0x80);write(chip,0x20+ch,0x09);write(chip,0x30+ch,0);}
        write(chip,0x0e,0x3f);
    } else {write(chip,0x30,mode==1?0:0x10);write(chip,0x10,0x80);write(chip,0x20,0x19);}
}
struct Reference {
    std::unique_ptr<OPLL,decltype(&OPLL_delete)> chip{OPLL_new(72,1),OPLL_delete};
    void writeIo(uint8_t port,uint8_t value) {OPLL_writeIO(chip.get(),port,value);}
};
bool clockTest() {
    Emu2413Native chip;
    std::array<int32_t,10> out;out.fill(123456);
    if(chip.masterTicks()!=0 || chip.advanceTo(0,{})!=0 || chip.advanceTo(71,{})!=0 || chip.masterTicks()!=71) return false;
    if(chip.advanceTo(72,out)!=1 || out[0]!=0 || out[1]!=123456 || chip.masterTicks()!=72) return false;
    if(chip.advanceTo(73,{}) || chip.advanceTo(143,{}) || chip.advanceTo(144,out)!=1) return false;
    if(chip.advanceTo(733,out)!=8 || chip.masterTicks()!=733 || out[8]!=123456) return false;
    chip.reset();
    constexpr uint64_t target=3579545ull*10+12345;
    std::array<int32_t,1024> block{};
    uint64_t time=0,total=0;
    while(time<target) {
        const auto next=std::min(target,time+70001);
        const auto n=chip.advanceTo(next,block);
        if(n!=next/72-time/72 || chip.masterTicks()!=next) return false;
        total+=n;time=next;
    }
    return total==497330 && chip.masterTicks()%72==35;
}
bool waveforms() {
    for(unsigned mode=0;mode<3;++mode) {
        Emu2413Native chip;Reference reference;
        if(!reference.chip || reference.chip->conv || reference.chip->out_step!=72 || reference.chip->inp_step!=72) return false;
        configure(chip,mode);configure(reference,mode);
        bool audible=false,released=false;
        std::array<int32_t,1> out{};
        for(uint64_t i=1;i<=50000;++i) {
            if(i==12500) {write(chip,mode==2?0x0e:0x20,mode==2?0x20:0x09);write(reference,mode==2?0x0e:0x20,mode==2?0x20:0x09);}
            if(chip.advanceTo(i*72-1,{}) || chip.advanceTo(i*72,out)!=1 || out[0]!=OPLL_calc(reference.chip.get())) return false;
            // The normalized upstream reference advances exactly once from the first call, no SRC.
            if(reference.chip->out_time!=0 || reference.chip->eg_counter!=i) return false;
            audible|=i<12500 && out[0]!=0;released|=i>12500 && out[0]!=0;
        }
        if(!audible || !released) return false;
        // Reset must undo the edited patch, rhythm and pending address as well as the master clock.
        chip.writeIo(0,7);chip.reset();Emu2413Native fresh;
        for(auto* p:{&chip,&fresh}) {
            p->writeIo(1,0x21); // Reset address is zero; do not rewrite the other user-patch bytes.
            write(*p,0x30,0);write(*p,0x10,0x80);write(*p,0x20,0x19);
        }
        std::vector<int32_t> a(4096),b(4096);
        if(chip.masterTicks()!=0 || chip.advanceTo(4096*72,a)!=4096 || fresh.advanceTo(4096*72,b)!=4096 || a!=b) return false;
    }
    return true;
}
bool partition() {
    Emu2413Native bulk,split;Reference reference;
    if(!reference.chip) return false;
    configure(bulk,0);configure(split,0);configure(reference,0);
    uint64_t time=0;
    std::vector<int32_t> all;
    for(uint64_t target:{71ull,72ull,73ull,7199ull,7200ull,7201ull,3579545ull}) {
        const auto expected=target/72-time/72;
        std::vector<int32_t> a(static_cast<size_t>(expected)),b(a.size());
        if(bulk.advanceTo(target,a)!=expected) return false;
        size_t count=0;
        for(uint64_t t=time;t<target;) {
            t=std::min(target,t+37);count+=split.advanceTo(t,std::span(b).subspan(count));
        }
        if(count!=a.size() || a!=b || split.masterTicks()!=target) return false;
        for(auto sample:a) if(sample!=OPLL_calc(reference.chip.get())) return false;
        all.insert(all.end(),a.begin(),a.end());
        // Two same-time DATA writes: the final value wins; updates at target preceded these writes.
        for(auto* p:{&bulk,&split}) {p->writeIo(0,0x10);p->writeIo(1,0x40);p->writeIo(1,0x80);}
        reference.writeIo(0,0x10);reference.writeIo(1,0x40);reference.writeIo(1,0x80);
        time=target;
    }
    return std::any_of(all.begin(),all.end(),[](auto sample){return sample!=0;});
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="clock"?clockTest():name=="waveforms"?waveforms():name=="partition"&&partition();
    if(!pass) std::cerr<<"Native FM test failed: "<<name<<'\n';
    return pass?0:1;
}
