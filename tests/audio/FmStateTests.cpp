#include "core/audio/Emu2413Native.h"

#include <array>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using namespace smsgg;
void write(Emu2413Native& chip,uint8_t address,uint8_t value) {chip.writeIo(0,address);chip.writeIo(1,value);}
void configure(Emu2413Native& chip,unsigned mode) {
    constexpr std::array<uint8_t,8> patch{0x71,0x61,0x1e,0x18,0xf2,0xf2,0x04,0x04};
    for(uint8_t i=0;i<patch.size();++i) write(chip,i,patch[i]);
    for(uint8_t ch=0;ch<9;++ch) {
        write(chip,0x30+ch,static_cast<uint8_t>((mode==1?0:(ch+1)<<4)|(ch%4)));
        write(chip,0x10+ch,static_cast<uint8_t>(0x80+ch*11));write(chip,0x20+ch,0x19);
    }
    if(mode==2) write(chip,0x0e,0x3f);
    write(chip,0x0f,mode==3?8:0);
}
void advance(Emu2413Native& chip,uint64_t time) {
    std::vector<int32_t> out(static_cast<size_t>(time/72-chip.masterTicks()/72));chip.advanceTo(time,out);
}
bool continueSame(Emu2413Native& original,Emu2413Native& restored) {
    const auto start=original.masterTicks();
    for(unsigned step=0;step<12;++step) {
        const auto at=start+step*113+1;
        std::array<int32_t,4> a{},b{};
        const auto n=original.advanceTo(at,a),m=restored.advanceTo(at,b);
        if(n!=m || a!=b || original.captureState()!=restored.captureState()) return false;
        const auto reg=static_cast<uint8_t>(step%3==0?0x0e:step%3==1?0x03:0x20+step%9);
        const auto data=static_cast<uint8_t>(step%3==0?0x20|step:step*31);
        write(original,reg,data);write(restored,reg,data);
    }
    return original.captureState()==restored.captureState();
}
bool roundtrip() {
    for(unsigned mode=0;mode<4;++mode) for(uint64_t at:{0ull,1ull,71ull,72ull,73ull,1729ull,140003ull}) {
        Emu2413Native original,restored;configure(original,mode);advance(original,at);
        original.writeIo(0,0xfd); // Full 8bit address latch, even above the register mask.
        const auto saved=original.captureState();
        if(saved.masterTicks!=at || saved.egCounter!=at/72 || saved.address!=0xfd || saved.chipType!=0
            || saved.testFlag!=(mode==3?8:0) || original.captureState()!=saved) return false;
        configure(restored,(mode+1)%4);advance(restored,500003);restored.restoreState(saved);
        if(restored.captureState()!=saved || !continueSame(original,restored)) return false;
        if(saved.masterTicks!=at || saved.address!=0xfd) return false;
        original.restoreState(saved);restored.restoreState(saved);
        if(original.captureState()!=saved || !continueSame(original,restored)) return false;
    }
    Emu2413Native low,high;configure(low,1);advance(low,1234567);
    auto epoch=low.captureState();epoch.masterTicks+=72ull*(uint64_t{1}<<32);high.restoreState(epoch);
    std::array<int32_t,4> a{},b{};
    const auto n=low.advanceTo(low.masterTicks()+200,a),m=high.advanceTo(epoch.masterTicks+200,b);
    auto highState=high.captureState(),lowState=low.captureState();
    if(n!=m || a!=b || highState.masterTicks<=UINT32_MAX) return false;
    highState.masterTicks=lowState.masterTicks;
    return highState==lowState;
}
bool pending() {
    for(bool oldWave:{false,true}) {
        const auto previousWave=static_cast<uint8_t>(oldWave),expectedWave=static_cast<uint8_t>(!oldWave);
        Emu2413Native original,restored;configure(original,1);
        write(original,3,oldWave?0x18:0);advance(original,4000*72);
        write(original,3,oldWave?0:0x18);write(original,0x20,0x1f);
        original.writeIo(0,0x07);
        const auto saved=original.captureState();
        if(saved.patches[0].ws!=expectedWave || saved.slots[0].wave!=previousWave
            || !(saved.slots[0].updateRequests&1) || saved.address!=7) return false;
        restored.restoreState(saved);
        if(restored.captureState()!=saved) return false;
        original.writeIo(1,0xa3);restored.writeIo(1,0xa3);
        advance(original,original.masterTicks()+72);advance(restored,restored.masterTicks()+72);
        const auto after=restored.captureState();
        if(after.slots[0].wave!=expectedWave || after.patches[0].ws!=expectedWave) return false;
        if(original.captureState()!=after || !continueSame(original,restored)) return false;
    }
    return true;
}
bool lifetime() {
    for(unsigned mode=0;mode<3;++mode) {
        auto source=std::make_unique<Emu2413Native>();configure(*source,mode);advance(*source,1234567);
        const auto saved=source->captureState();
        std::vector<int32_t> expected(12000),actual(12000);
        const auto target=(saved.masterTicks/72+expected.size())*72+13;
        if(source->advanceTo(target,expected)!=expected.size()) return false;
        source.reset();
        // Occupy a new chip/patch allocation after the original owner has gone away.
        auto other=std::make_unique<Emu2413Native>();configure(*other,(mode+1)%3);advance(*other,99999);
        Emu2413Native restored;configure(restored,(mode+2)%3);advance(restored,777777);
        restored.restoreState(saved);
        if(restored.captureState()!=saved || restored.advanceTo(target,actual)!=actual.size() || actual!=expected) return false;
        bool audible=false;for(auto sample:actual) audible|=sample!=0;if(!audible) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="roundtrip"?roundtrip():name=="pending"?pending():name=="lifetime"&&lifetime();
    if(!pass) std::cerr<<"FM state failed: "<<name<<'\n';
    return pass?0:1;
}
