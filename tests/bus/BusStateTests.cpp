#include "core/bus/MemoryBus.h"
#include "core/bus/MemoryControl.h"
#include "core/input/Controller.h"
#include "../cpu/Z80MemoryTestSupport.h"
#include "IoTestSupport.h"

#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;
bool memory() {
    test::Memory firstExternal,secondExternal;
    firstExternal.bytes[0]=0x11;secondExternal.bytes[0]=0x22;
    MemoryBus first(firstExternal),second(secondExternal);
    for(unsigned offset=0;offset<8192;++offset) first.write8(static_cast<uint16_t>(0xc000+offset),static_cast<uint8_t>(offset*7+offset/256));
    for(const uint8_t control:{0,0x10,0x40,0x50}) {
        first.setMemoryControl(control);const auto saved=first.captureState();
        if(saved.ramEnabled!=((control&0x10)==0) || saved.cartridgeEnabled!=((control&0x40)==0)) return false;
        second.setMemoryControl(control^0x50);second.restoreState(saved);
        if(second.read8(0)!=((control&0x40)?0xff:0x22)) return false;
        for(unsigned offset=0;offset<8192;++offset) {
            const auto expected=(control&0x10)?0xff:static_cast<uint8_t>(offset*7+offset/256);
            if(second.read8(static_cast<uint16_t>(0xc000+offset))!=expected
                || second.read8(static_cast<uint16_t>(0xe000+offset))!=expected) return false;
        }
        firstExternal.accesses.clear();secondExternal.accesses.clear();
        second.write8(0x1234,0x5a);
        if(!firstExternal.accesses.empty() || ((control&0x40)?!secondExternal.accesses.empty():!secondExternal.accessedOnce(true,0x1234,0x5a))) return false;
        second.setMemoryControl(0);
        for(unsigned offset=0;offset<8192;++offset)
            if(second.read8(static_cast<uint16_t>(0xe000+offset))!=static_cast<uint8_t>(offset*7+offset/256)) return false;
        second.write8(0xc001,0xaa);
        if(saved.ram[1]!=7) return false;
    }
    first.setMemoryControl(0);const auto saved=first.captureState();
    first.write8(0xc000,0x99);second.restoreState(saved);
    return second.read8(0xe000)==0;
}
bool control() {
    for(const auto machine:{Machine::Sms,Machine::GameGear}) for(unsigned value=0;value<256;++value) {
        test::Memory externalA,externalB;externalA.bytes[0]=0x11;externalB.bytes[0]=0x22;
        MemoryBus a(externalA),b(externalB);test::Io portsA,portsB;portsA.reply=portsB.reply=0x9b;
        MemoryControl original(a,portsA,machine),restored(b,portsB,machine);
        original.writePort(0x3e,static_cast<uint8_t>(value));
        const auto saved=original.captureState();const auto bus=a.captureState();
        original.writePort(0x3e,0);restored.writePort(0x3e,0x54);
        b.restoreState(bus);restored.restoreState(saved);
        const bool ioOff=machine==Machine::Sms && (value&4);
        if(saved.ioDisabled!=ioOff || restored.readPort(0xdc)!=(ioOff?0xff:0x9b)
            || restored.readPort(0xdd)!=(ioOff?0xff:0x9b) || restored.readPort(0xbe)!=0x9b) return false;
        portsA.accesses.clear();portsB.accesses.clear();restored.writePort(0x3f,0x35);
        if(!portsA.accesses.empty() || portsB.accesses.size()!=(ioOff?0u:1u)) return false;
        if(b.read8(0)!=((machine==Machine::Sms && (value&0x40))?0xff:0x22)) return false;
        b.write8(0xc000,0x66);
        if(b.read8(0xe000)!=((value&0x10)?0xff:0x66)) return false;
        // Restoring the control owner alone must not replay 3E into MemoryBus.
        b.setMemoryControl(0);restored.restoreState(saved);
        if(b.read8(0)!=0x22) return false;
    }
    return true;
}
bool controller() {
    for(unsigned buttons=0;buttons<128;++buttons) for(unsigned latch=0;latch<256;++latch) {
        const PadState pad{bool(buttons&1),bool(buttons&2),bool(buttons&4),bool(buttons&8),bool(buttons&16),bool(buttons&32),bool(buttons&64)};
        Controller original;original.setState(pad);original.writeControl(static_cast<uint8_t>(latch));
        const auto dc=original.readDc(),dd=original.readDd();const bool start=original.startPressed();
        const auto saved=original.captureState();original.setState({});original.writeControl(0);
        Controller restored;restored.setState({true,true,true,true,true,true,true});restored.writeControl(0xff);
        restored.restoreState(saved);
        if(saved.control!=latch || restored.readDc()!=dc || restored.readDd()!=dd || restored.startPressed()!=start) return false;
        const auto copy=restored.captureState().pad;
        if(copy.up!=pad.up || copy.down!=pad.down || copy.left!=pad.left || copy.right!=pad.right
            || copy.button1!=pad.button1 || copy.button2!=pad.button2 || copy.start!=pad.start) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2) {
        const std::string_view name(argv[1]);
        if(name=="memory"?memory():name=="control"?control():name=="controller" && controller()) return 0;
        std::cerr<<"Bus state "<<name<<" failed\n";
    }
    return 1;
}
