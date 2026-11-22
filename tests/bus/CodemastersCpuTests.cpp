#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"
#include "core/bus/MemoryBus.h"
#include "IoTestSupport.h"

#include <algorithm>
#include <exception>
#include <iostream>

int main() try {
    using namespace smsgg;
    std::vector<uint8_t> bytes(65536);
    for(size_t at=0;at<bytes.size();++at) bytes[at]=static_cast<uint8_t>(at/16384+0x40);
    bytes[3*16384+0x100]=0x3e;bytes[3*16384+0x101]=0x77;bytes[3*16384+0x102]=0x76;
    Cartridge cartridge(std::get<RomImage>(RomLoader::fromBytes(bytes,{MapperKind::Codemasters})));
    MemoryBus memory(cartridge);
    if(memory.read8(0x8000)!=0x40) {std::cerr<<"Initial slot 2 is not bank 0\n";return 1;}
    // Run from internal RAM while replacing both interrupt vectors and executable slot 2.
    const uint8_t code[]{0x3e,3,0x32,0,0,0x3a,0x38,0,0x32,0,0xc0,
        0x3e,3,0x32,0xff,0xbf,0xc3,0,0x81};
    for(size_t i=0;i<std::size(code);++i) memory.write8(static_cast<uint16_t>(0xc100+i),code[i]);
    Z80 cpu({.sp=0xdff8,.pc=0xc100});test::Io io;
    for(unsigned step=0;step<10;++step) if(!std::holds_alternative<uint32_t>(cpu.step(memory,io))) return 1;
    if(memory.read8(0)!=0x43 || memory.read8(0xc000)!=0x43 || cpu.registers().a()!=0x77) return 1;
    memory.write8(0xffff,1);memory.write8(0xfffd,1);
    if(memory.read8(0)!=0x43 || memory.read8(0x8000)!=0x43 || memory.read8(0xffff)!=1) return 1;
    memory.setMemoryControl(0x40);memory.write8(0,1);
    if(memory.read8(0)!=0xff) return 1;
    memory.setMemoryControl(0);if(memory.read8(0)!=0x43) return 1;
    memory.setMemoryControl(0x10);memory.write8(0x8000,2);memory.setMemoryControl(0);
    if(memory.read8(0x8000)!=0x42 || memory.read8(0xc000)!=0x43) return 1;
    cartridge.reset();if(memory.read8(0)!=0x40 || memory.read8(0x4000)!=0x41 || memory.read8(0x8000)!=0x40) return 1;
    Cartridge ram(std::get<RomImage>(RomLoader::fromBytes(bytes,{MapperKind::Codemasters,8192})));
    MemoryBus ramBus(ram);
    const uint8_t ramCode[]{
        0x3e,0x81,0x32,0,0x40,0x3e,2,0x32,0,0x80,
        0x3e,0x55,0x32,0,0xa0,0x3e,0xaa,0x32,0xff,0xbf,
        0x3a,0,0xa0,0x32,0x20,0xc0,0x3e,1,0x32,0,0x40,
        0x3a,0,0x80,0x32,0x21,0xc0,0x76};
    for(size_t i=0;i<std::size(ramCode);++i) ramBus.write8(static_cast<uint16_t>(0xc100+i),ramCode[i]);
    Z80 ramCpu({.sp=0xdff8,.pc=0xc100});
    for(unsigned step=0;step<20;++step) if(!std::holds_alternative<uint32_t>(ramCpu.step(ramBus,io))) return 1;
    if(ramBus.read8(0xc020)!=0x55 || ramBus.read8(0xc021)!=0x42 || ramBus.read8(0xbfff)!=0x42) return 1;
    ramBus.write8(0x4000,0x81);
    ramBus.setMemoryControl(0x40);ramBus.write8(0xa000,0);
    if(ramBus.read8(0xa000)!=0xff) return 1;
    ramBus.setMemoryControl(0);
    if(ramBus.read8(0xa000)!=0x55 || ramBus.read8(0xbfff)!=0xaa) return 1;
    ramBus.setMemoryControl(0x10);ramBus.write8(0xa001,0x66);ramBus.setMemoryControl(0);
    if(ramBus.read8(0xa001)!=0x66) return 1;
    ram.reset();
    if(ramBus.read8(0xa000)!=0x40) return 1;
    ramBus.write8(0x4000,0x81);
    if(ramBus.read8(0xa000)!=0x55 || ramBus.read8(0xa001)!=0x66 || ramBus.read8(0xbfff)!=0xaa) return 1;
    return 0;
} catch(const std::exception& e) {
    std::cerr<<e.what()<<'\n';return 1;
}
