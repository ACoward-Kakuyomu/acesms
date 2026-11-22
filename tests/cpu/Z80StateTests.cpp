#include "Z80MemoryTestSupport.h"
#include "../bus/IoTestSupport.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;
using test::Memory;
using test::run;
bool same(const Z80State& a,const Z80State& b) {
    return test::sameRegisters(a.registers,b.registers) && a.iff1==b.iff1 && a.iff2==b.iff2
        && a.halted==b.halted && a.flagsModified==b.flagsModified && a.ldAir==b.ldAir
        && a.eiDelay==b.eiDelay && a.interruptMode==b.interruptMode && a.memptr==b.memptr;
}
bool sameMemory(const Memory& a,const Memory& b) {
    if(a.bytes!=b.bytes || a.accesses.size()!=b.accesses.size()) return false;
    for(size_t i=0;i<a.accesses.size();++i) {
        const auto& x=a.accesses[i];const auto& y=b.accesses[i];
        if(x.write!=y.write || x.address!=y.address || x.value!=y.value) return false;
    }
    return true;
}
bool roundtrip() {
    constexpr uint8_t code[]{0xfb,0x06,0x27,0x04,0x32,0,0x40,0xd3,0x7f,0xed,0x57,0x76};
    for(unsigned boundary=0;boundary<=7;++boundary) {
        auto registers=test::initialRegisters();registers.af=0;registers.i=0x28;registers.r=0x7f;
        Z80 original(registers);Memory memory;test::Io io;io.reply=0x96;
        std::copy(std::begin(code),std::end(code),memory.bytes.begin()+registers.pc);
        for(unsigned n=0;n<boundary;++n) if(!std::holds_alternative<uint32_t>(original.step(memory,io))) return false;
        const auto saved=original.captureState();
        if(!test::sameRegisters(saved.registers,original.registers()) || !same(saved,original.captureState())) return false;
        Memory other=memory;test::Io otherIo;otherIo.reply=io.reply;
        Z80 restored({});
        if(!run(restored,{0xed,0x5e},&other) || !run(restored,{0xfb},&other) || !run(restored,{0xed,0x5f},&other)) return false;
        if(boundary%2==0 && !run(restored,{0xfb},&other)) return false;
        restored.restoreState(saved);
        if(!same(saved,restored.captureState())) return false;
        for(unsigned n=0;n<6;++n) {
            memory.accesses.clear();other.accesses.clear();io.accesses.clear();otherIo.accesses.clear();
            const auto expected=original.step(memory,io),actual=restored.step(other,otherIo);
            if(expected!=actual || !std::holds_alternative<uint32_t>(actual)
                || !same(original.captureState(),restored.captureState()) || !sameMemory(memory,other)
                || io.accesses.size()!=otherIo.accesses.size()) return false;
            for(size_t i=0;i<io.accesses.size();++i) {
                const auto& x=io.accesses[i];const auto& y=otherIo.accesses[i];
                if(x.write!=y.write || x.port!=y.port || x.value!=y.value) return false;
            }
        }
        original.reset();restored.restoreState(saved);
        if(!same(saved,restored.captureState())) return false;
    }
    return true;
}
bool interrupts() {
    constexpr std::array<uint8_t,1> rst{0xff};
    for(unsigned mode=0;mode<3;++mode) {
        auto registers=test::initialRegisters();registers.sp=0;registers.pc=0xffff;
        Z80 original(registers);Memory a;
        a.bytes[(registers.i<<8)|0xff]=0x34;a.bytes[(registers.i+1)<<8]=0x12;
        if(!run(original,{0xed,static_cast<uint8_t>(mode==0?0x46:mode==1?0x56:0x5e)},&a)
            || !run(original,{0xfb},&a)) return false;
        Z80 restored({});restored.restoreState(original.captureState());Memory b=a;
        if(original.canAcceptMaskableInterrupt() || restored.canAcceptMaskableInterrupt()
            || original.acceptInterrupt(rst,&a)!=0 || restored.acceptInterrupt(rst,&b)!=0) return false;
        if(!run(original,{0x76},&a) || !run(restored,{0x76},&b)) return false;
        const auto halted=original.captureState();restored.reset();restored.restoreState(halted);
        if(!restored.halted() || !restored.canAcceptMaskableInterrupt()) return false;
        a.accesses.clear();b.accesses.clear();
        const auto expected=original.acceptInterrupt(rst,&a),actual=restored.acceptInterrupt(rst,&b);
        if(expected!=actual || actual!=(mode==2?19u:13u) || restored.halted()
            || restored.registers().pc!=(mode==2?0x1234:0x38)
            || !same(original.captureState(),restored.captureState()) || !sameMemory(a,b)) return false;
    }
    Z80 original(test::initialRegisters());Memory a;
    if(!run(original,{0xfb},&a) || !run(original,{0x76},&a) || original.acceptNmi(a)!=11) return false;
    Z80 restored({});restored.restoreState(original.captureState());Memory b=a;
    if(restored.iff1() || !restored.iff2() || restored.halted()) return false;
    a.accesses.clear();b.accesses.clear();
    if(run(original,{0xed,0x45},&a)!=14 || run(restored,{0xed,0x45},&b)!=14
        || !restored.iff1() || !same(original.captureState(),restored.captureState()) || !sameMemory(a,b)) return false;
    return true;
}
bool hidden() {
    Memory memory;
    for(bool flagHistory:{false,true}) {
        auto registers=test::initialRegisters();registers.af=0;registers.bc=0x2700;
        Z80 original(registers);
        if(!run(original,{0x04},&memory) || (!flagHistory && !run(original,{0},&memory))) return false;
        Z80 restored({});restored.restoreState(original.captureState());
        if(!run(original,{0x37},&memory) || !run(restored,{0x37},&memory)
            || restored.registers().f()!=(flagHistory?0x01:0x29)
            || !same(original.captureState(),restored.captureState())) return false;
    }
    auto registers=test::initialRegisters();registers.af=0;
    Z80 original(registers);
    if(!run(original,{0xc3,0x10,0x28},&memory)) return false;
    Z80 restored({});restored.restoreState(original.captureState());memory.bytes[registers.hl]=0;
    if(!run(original,{0xcb,0x46},&memory) || !run(restored,{0xcb,0x46},&memory)
        || restored.registers().f()!=0x7c || !same(original.captureState(),restored.captureState())) return false;
    for(const uint8_t opcode:{0x57,0x5f}) {
        Z80 before(registers);
        if(!run(before,{0xed,0x56},&memory) || !run(before,{0xfb},&memory) || !run(before,{0xed,opcode},&memory)) return false;
        const auto flags=before.registers().f();if(!(flags&4)) return false;
        Z80 after({});after.restoreState(before.captureState());Memory other=memory;
        if(before.acceptInterrupt({},&memory)!=13 || after.acceptInterrupt({},&other)!=13
            || after.registers().f()!=(flags&0xfb) || !same(before.captureState(),after.captureState())) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2) {
        const std::string_view name(argv[1]);
        const bool ok=name=="roundtrip"?roundtrip():name=="interrupts"?interrupts():name=="hidden" && hidden();
        if(ok) return 0;
        std::cerr<<"CPU state "<<name<<" failed\n";
    }
    return 1;
}
