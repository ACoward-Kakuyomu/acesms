#include "core/cartridge/CodemastersMapper.h"
#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"

#include <algorithm>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;
RomImage image(size_t size,size_t ramBytes=0) {
    std::vector<uint8_t> bytes(size);
    for(size_t i=0;i<size;++i) bytes[i]=static_cast<uint8_t>((i/16384)*19+(i%16384)*7+(i%16384)/256);
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::Codemasters,ramBytes}));
}
uint8_t expected(const RomImage& rom,unsigned bank,unsigned offset) {
    return rom.bytes()[(bank%(rom.bytes().size()/16384))*16384+offset];
}
bool rom() {
    for(const size_t size:{32768,65536,131072,262144,524288,1048576}) {
        const auto bytes=image(size);
        CodemastersMapper mapper(bytes);
        for(unsigned slot=0;slot<3;++slot) for(unsigned bank=0;bank<(slot==1?128:256);++bank) {
            mapper.reset();mapper.write8(static_cast<uint16_t>(slot*16384),static_cast<uint8_t>(bank));
            for(const unsigned offset:{0,1,0x3ff,0x400,0x1fff,0x2000,0x3ffe,0x3fff}) {
                const auto address=static_cast<uint16_t>(slot*16384+offset);
                if(mapper.read8(address)!=expected(bytes,bank,offset)) return false;
            }
            for(unsigned other=0;other<3;++other) if(other!=slot) {
                const unsigned initial=other==1?1:0;
                if(mapper.read8(static_cast<uint16_t>(other*16384+0x123))!=expected(bytes,initial,0x123)) return false;
            }
        }
    }
    return true;
}
bool mirrors() {
    const auto bytes=image(65536);CodemastersMapper mapper(bytes);
    for(unsigned address=0;address<0xc000;++address) {
        mapper.reset();mapper.write8(static_cast<uint16_t>(address),3);
        const unsigned slot=address/16384;
        for(unsigned other=0;other<3;++other) {
            const unsigned bank=other==slot?3:other==1?1:0;
            if(mapper.read8(static_cast<uint16_t>(other*16384+0x3ff))!=expected(bytes,bank,0x3ff)) return false;
        }
    }
    mapper.reset();
    for(unsigned address=0xc000;address<65536;++address) {
        mapper.write8(static_cast<uint16_t>(address),3);
        if(mapper.read8(static_cast<uint16_t>(address))!=0xff) return false;
    }
    for(unsigned address=0;address<0xc000;++address) {
        const unsigned bank=address/16384==1?1:0;
        if(mapper.read8(static_cast<uint16_t>(address))!=expected(bytes,bank,address%16384)) return false;
    }
    return true;
}
bool reset() {
    const auto bytes=image(65536);CodemastersMapper mapper(bytes);
    for(unsigned slot=0;slot<3;++slot) mapper.write8(static_cast<uint16_t>(slot*16384),3);
    mapper.reset();
    for(unsigned address=0;address<0xc000;++address) {
        const unsigned bank=address/16384==1?1:0;
        if(mapper.read8(static_cast<uint16_t>(address))!=expected(bytes,bank,address%16384)) return false;
    }
    // The immutable source must never be changed by writes.
    const auto original=image(65536);
    return std::equal(bytes.bytes().begin(),bytes.bytes().end(),original.bytes().begin());
}
bool ram() {
    for(const size_t capacity:{0,8192}) {
        const auto bytes=image(1048576,capacity);
        Cartridge cartridge(bytes);
        cartridge.write8(0x8000,2);
        cartridge.write8(0x4000,0x81);
        if(cartridge.read8(0xa000)!=(capacity?0:0xff)) {
            std::cerr<<"RAM window is not mapped, capacity="<<capacity<<'\n';return false;
        }
        // Every RAM byte is distinct from its ROM offset; all writes must preserve bank 2.
        for(unsigned offset=0;offset<8192;++offset)
            cartridge.write8(static_cast<uint16_t>(0xa000+offset),static_cast<uint8_t>(offset*13+offset/256));
        for(unsigned offset=0;offset<8192;++offset) {
            const auto value=capacity?static_cast<uint8_t>(offset*13+offset/256):0xff;
            if(cartridge.read8(static_cast<uint16_t>(0xa000+offset))!=value
                || cartridge.read8(static_cast<uint16_t>(0x8000+offset))!=expected(bytes,2,offset)) return false;
        }
        // Every lower half write remains a bank mirror even while RAM is enabled.
        for(unsigned address=0x8000;address<0xa000;++address) {
            const auto bank=static_cast<uint8_t>(address);
            cartridge.write8(static_cast<uint16_t>(address),bank);
            if(cartridge.read8(0x9fff)!=expected(bytes,bank,0x1fff)) return false;
        }
        cartridge.write8(0x8000,3);
        // Slot 1 mirrors update ROM bank and RAM gate together, for all bank values.
        for(unsigned value=0;value<256;++value) {
            cartridge.write8(0x7fff,static_cast<uint8_t>(value));
            if(cartridge.read8(0x4000)!=expected(bytes,value&0x7f,0)) return false;
            const auto upper=(value&0x80)?(capacity?uint8_t(0):uint8_t(0xff)):expected(bytes,3,0x2000);
            if(cartridge.read8(0xa000)!=upper) return false;
        }
        for(unsigned address=0x4000;address<0x8000;++address) {
            const auto value=static_cast<uint8_t>(address);
            cartridge.write8(static_cast<uint16_t>(address),value);
            if(cartridge.read8(0x5fff)!=expected(bytes,value&0x7f,0x1fff)) return false;
            if(cartridge.read8(0xbfff)!=((value&0x80)?(capacity?uint8_t(0x12):uint8_t(0xff)):expected(bytes,3,0x3fff))) return false;
        }
        cartridge.write8(0x4000,1);
        if(cartridge.read8(0xa000)!=expected(bytes,3,0x2000)) return false;
        cartridge.write8(0xbfff,4); // Disabled RAM restores the upper bank-write mirrors.
        if(cartridge.read8(0x8000)!=expected(bytes,4,0)) return false;
        cartridge.reset();
        if(cartridge.read8(0)!=expected(bytes,0,0) || cartridge.read8(0x4000)!=expected(bytes,1,0)
            || cartridge.read8(0x8000)!=expected(bytes,0,0) || cartridge.read8(0xbfff)!=expected(bytes,0,0x3fff)) return false;
        cartridge.write8(0x4000,0x81);
        for(unsigned offset=0;offset<8192;++offset) {
            const auto value=capacity?static_cast<uint8_t>(offset*13+offset/256):0xff;
            if(cartridge.read8(static_cast<uint16_t>(0xa000+offset))!=value) return false;
        }
        for(unsigned bank=0;bank<64;++bank) {
            cartridge.write8(0,static_cast<uint8_t>(bank));
            for(unsigned offset=0;offset<16384;++offset)
                if(cartridge.read8(static_cast<uint16_t>(offset))!=expected(bytes,bank,offset)) return false;
        }
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2) {
        const std::string_view name(argv[1]);
        if(name=="rom") return rom()?0:1;
        if(name=="mirrors") return mirrors()?0:1;
        if(name=="reset") return reset()?0:1;
        if(name=="ram") return ram()?0:1;
    }
    return 1;
}
