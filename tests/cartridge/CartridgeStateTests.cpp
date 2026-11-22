#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;
RomImage image(MapperKind kind,size_t capacity=0,size_t size=65536) {
    std::vector<uint8_t> bytes(size);
    for(size_t i=0;i<size;++i) bytes[i]=static_cast<uint8_t>((i/16384)*23+(i%16384)*3+i/256);
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{kind,capacity}));
}
bool sameReads(Cartridge& a,Cartridge& b) {
    for(unsigned at=0;at<65536;++at) if(a.read8(static_cast<uint16_t>(at))!=b.read8(static_cast<uint16_t>(at))) return false;
    return true;
}
bool linear() {
    for(const size_t size:{8192,16384,32768,49152}) {
        const auto rom=image(MapperKind::RomOnly,0,size);Cartridge original(rom),restored(rom);
        const auto saved=original.captureState();
        if(!saved.ram.empty() || !std::holds_alternative<std::monostate>(saved.mapper)) return false;
        original.write8(0,0xff);restored.restoreState(saved);
        if(!sameReads(original,restored)) return false;
    }
    return true;
}
bool sega() {
    for(const size_t capacity:{0,8192,16384,32768}) {
        const auto rom=image(MapperKind::Sega,capacity);Cartridge original(rom),restored(rom);
        std::vector<uint8_t> expected(capacity);
        for(unsigned bank=0;bank<2;++bank) {
            original.write8(0xfffc,static_cast<uint8_t>(8+bank*4));
            for(unsigned offset=0;offset<16384;++offset) {
                const auto value=static_cast<uint8_t>(bank*29+offset*7+offset/256);
                original.write8(static_cast<uint16_t>(0x8000+offset),value);
                if(capacity) expected[(bank*16384+offset)%capacity]=value;
            }
        }
        original.write8(0xfffd,0x83);original.write8(0xfffe,0x82);original.write8(0xffff,0x81);
        original.write8(0xfffc,0xfd);
        const auto saved=original.captureState();const auto& mapper=std::get<SegaMapperState>(saved.mapper);
        if(saved.ram!=expected || mapper.banks!=std::array<uint8_t,3>{0x83,0x82,0x81} || mapper.control!=0xfd) return false;
        restored.write8(0xfffd,1);restored.restoreState(saved);
        if(!sameReads(original,restored)) return false;
        if(capacity) {
            const auto first=restored.read8(0x8000),second=original.read8(0x8001);
            original.write8(0x8000,static_cast<uint8_t>(first^0xff));
            restored.write8(0x8001,static_cast<uint8_t>(second^0xff));
            if(restored.read8(0x8000)!=first || original.read8(0x8001)!=second || saved.ram!=expected) return false;
        }
        restored.reset();restored.restoreState(saved);
        if(restored.captureState().ram!=expected) return false;
        restored.write8(0xfffc,0);
        if(restored.read8(0)!=rom.bytes()[0] || restored.read8(0x400)!=rom.bytes()[3*16384+0x400]
            || restored.read8(0x4000)!=rom.bytes()[2*16384] || restored.read8(0x8000)!=rom.bytes()[16384]) return false;
        restored.write8(0xfffc,0xfd);
        if(capacity && restored.read8(0x8000)!=expected[16384%capacity]) return false;
        if(!capacity && restored.read8(0x8000)!=0xff) return false;
    }
    return true;
}
bool codemasters() {
    for(const size_t capacity:{0,8192}) {
        const auto rom=image(MapperKind::Codemasters,capacity);Cartridge original(rom),restored(rom);
        original.write8(0,0xff);original.write8(0x8000,0xfe);original.write8(0x4000,0x82);
        std::vector<uint8_t> expected(capacity);
        for(unsigned offset=0;offset<8192;++offset) {
            const auto value=static_cast<uint8_t>(17+offset*5+offset/256);
            original.write8(static_cast<uint16_t>(0xa000+offset),value);
            if(capacity) expected[offset]=value;
        }
        const auto saved=original.captureState();const auto& mapper=std::get<CodemastersMapperState>(saved.mapper);
        if(saved.ram!=expected || mapper.banks!=std::array<uint8_t,3>{0xff,2,0xfe} || !mapper.ramEnabled) return false;
        restored.write8(0x4000,3);restored.restoreState(saved);
        if(!sameReads(original,restored)) return false;
        if(capacity) {
            original.write8(0xa000,0xff);restored.write8(0xa001,0xee);
            if(restored.read8(0xa000)!=17 || original.read8(0xa001)!=22 || saved.ram!=expected) return false;
        }
        restored.reset();restored.restoreState(saved);
        if(restored.captureState().ram!=expected) return false;
        const auto upper=restored.read8(0xbfff);
        restored.write8(0x9fff,3);
        if(restored.read8(0x8000)!=rom.bytes()[3*16384] || restored.read8(0xbfff)!=upper) return false;
        restored.write8(0x4000,2);
        if(restored.read8(0xbfff)!=rom.bytes()[3*16384+0x3fff]) return false;
        restored.restoreState(saved);
        if(restored.read8(0x8000)!=rom.bytes()[2*16384] || restored.read8(0xbfff)!=upper) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc==2) {
        const std::string_view name(argv[1]);
        if(name=="linear"?linear():name=="sega"?sega():name=="codemasters" && codemasters()) return 0;
        std::cerr<<"Cartridge state "<<name<<" failed\n";
    }
    return 1;
}
