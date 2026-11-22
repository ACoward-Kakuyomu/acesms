#include "core/bus/MemoryBus.h"
#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"
#include "IoTestSupport.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <algorithm>
#include <iostream>
#include <string_view>

using smsgg::MemoryBus;
using smsgg::test::Memory;

namespace {
bool initial() {
    Memory external;
    MemoryBus first(external), second(external);
    for (unsigned offset = 0; offset < 8192; ++offset) {
        if (first.read8(static_cast<uint16_t>(0xc000 + offset)) != 0
            || first.read8(static_cast<uint16_t>(0xe000 + offset)) != 0) return false;
        first.write8(static_cast<uint16_t>(0xc000 + offset), 0xff);
        if (second.read8(static_cast<uint16_t>(0xe000 + offset)) != 0) return false;
    }
    return true;
}

bool mirrors() {
    Memory external;
    MemoryBus bus(external);
    for (const unsigned pattern : {0, 0x55, 0xaa, 0xff}) {
        for (const unsigned base : {0xc000, 0xe000}) {
            for (unsigned offset = 0; offset < 8192; ++offset) {
                bus.write8(static_cast<uint16_t>(base + offset), static_cast<uint8_t>((offset ^ (offset / 256)) ^ pattern));
            }
            external.accesses.clear();
            for (unsigned offset = 0; offset < 8192; ++offset) {
                const uint8_t expected = static_cast<uint8_t>((offset ^ (offset / 256)) ^ pattern);
                if (bus.read8(static_cast<uint16_t>(0xc000 + offset)) != expected
                    || bus.read8(static_cast<uint16_t>(0xe000 + offset)) != expected) return false;
            }
            if (!external.accesses.empty()) return false;
        }
    }
    return true;
}

bool boundaries() {
    Memory external;
    MemoryBus bus(external);
    external.bytes[0xbfff] = 0xa5;
    if (bus.read8(0xbfff) != 0xa5 || !external.accessedOnce(false, 0xbfff, 0xa5)) return false;
    bus.write8(0xc000, 0x12); bus.write8(0xdfff, 0x34);
    if (bus.read8(0xe000) != 0x12 || bus.read8(0xffff) != 0x34) return false;
    bus.write8(0xe000, 0x56); bus.write8(0xffff, 0x78);
    return bus.read8(0xc000) == 0x56 && bus.read8(0xdfff) == 0x78 && bus.read8(0xbfff) == 0xa5;
}
bool externalRead() {
    Memory external;
    MemoryBus bus(external);
    for (const unsigned pattern : {0, 0x55, 0xaa, 0xff}) {
        for (unsigned address = 0; address < 0xc000; ++address) {
            const auto location = static_cast<uint16_t>(address);
            const auto value = static_cast<uint8_t>((address ^ (address / 256)) ^ pattern);
            external.bytes[location] = value; external.accesses.clear();
            if (bus.read8(location) != value || !external.accessedOnce(false, location, value)) return false;
        }
    }
    return true;
}

bool externalWrite() {
    Memory external;
    MemoryBus bus(external);
    for (const unsigned pattern : {0, 0x55, 0xaa, 0xff}) {
        for (unsigned address = 0; address < 65536; ++address) {
            const auto location = static_cast<uint16_t>(address);
            const auto value = static_cast<uint8_t>((address ^ (address / 256)) ^ pattern);
            external.accesses.clear(); bus.write8(location, value);
            if (!external.accessedOnce(true, location, value)) return false;
            if (address >= 0xc000 && (bus.read8(location) != value || external.accesses.size() != 1)) return false;
            if (pattern == 0 && address == 0xbfff) {
                for (unsigned offset = 0; offset < 8192; ++offset) {
                    if (bus.read8(static_cast<uint16_t>(0xc000 + offset)) != 0) return false;
                }
                if (external.accesses.size() != 1) return false;
            }
        }
    }
    return true;
}

bool overlap() {
    Memory external;
    MemoryBus bus(external);
    for (unsigned i = 0; i < 4; ++i) {
        const auto high = static_cast<uint16_t>(0xfffc + i), low = static_cast<uint16_t>(0xdffc + i);
        external.accesses.clear(); bus.write8(high, static_cast<uint8_t>(0x42 + i));
        if (!external.accessedOnce(true, high, static_cast<uint8_t>(0x42 + i))) return false;
        external.bytes[high] = 0xa5; // External state is not RAM readback.
        external.accesses.clear();
        if (bus.read8(high) != 0x42 + i || bus.read8(low) != 0x42 + i || !external.accesses.empty()) return false;
        bus.write8(low, static_cast<uint8_t>(0x91 + i));
        if (!external.accessedOnce(true, low, static_cast<uint8_t>(0x91 + i))
            || bus.read8(high) != 0x91 + i || external.bytes[high] != 0xa5) return false;
    }
    // A device observing a RAM write sees the RAM already updated.
    class Observer final : public smsgg::Z80Memory {
    public:
        MemoryBus* bus{};
        bool visible{};
        uint8_t read8(uint16_t) override { return 0; }
        void write8(uint16_t address, uint8_t value) override { visible = bus->read8(address) == value; }
    } observer;
    MemoryBus observed(observer); observer.bus = &observed;
    observed.write8(0xc000, 0x5a);
    return observer.visible;
}
bool sg1000Map() {
    Memory external;
    MemoryBus bus(external,smsgg::Machine::Sg1000);
    external.bytes[0xbfff]=0xa5;
    bus.setMemoryControl(0x50);
    if(bus.read8(0xbfff)!=0xa5 || !external.accessedOnce(false,0xbfff,0xa5)) return false;
    external.accesses.clear();
    for(unsigned offset=0;offset<0x400;++offset)
        bus.write8(static_cast<uint16_t>(0xc000+offset),static_cast<uint8_t>(offset/256+offset));
    for(unsigned address=0xc000;address<=0xffff;++address) {
        const auto offset=address&0x3ff;
        if(bus.read8(static_cast<uint16_t>(address))!=static_cast<uint8_t>(offset/256+offset)) return false;
    }
    bus.write8(0xffff,0x77);
    bus.write8(0xfffc,0x08);
    if(bus.read8(0xc3ff)!=0x77 || bus.read8(0xc3fc)!=0x08 || !external.accesses.empty()) return false;
    bus.write8(0xbfff,0x92);
    return external.accessedOnce(true,0xbfff,0x92);
}
bool sg1000Fetch() {
    std::vector<uint8_t> bytes(49152);
    constexpr uint8_t program[]{
        0x3a,0xff,0xbf, // LD A,($BFFF)
        0x32,0x00,0xc0, // LD ($C000),A
        0x3e,0x00,      // LD A,0
        0x3a,0x00,0xfc  // LD A,($FC00)
    };
    std::ranges::copy(program,bytes.begin());
    bytes.back()=0x5a;
    auto image=smsgg::RomLoader::fromBytes(std::move(bytes),
        {.mapper = smsgg::MapperKind::RomOnly, .machine = smsgg::Machine::Sg1000});
    smsgg::Cartridge cartridge(std::get<smsgg::RomImage>(std::move(image)));
    MemoryBus bus(cartridge,smsgg::Machine::Sg1000);
    smsgg::test::Io io;
    smsgg::Z80 cpu(smsgg::Z80Registers{});
    for(const uint32_t expected:{13,13,7,13}) {
        const auto elapsed=cpu.step(bus,io);
        if(!std::holds_alternative<uint32_t>(elapsed) || std::get<uint32_t>(elapsed)!=expected) return false;
    }
    return cpu.registers().pc==std::size(program) && cpu.registers().a()==0x5a && bus.read8(0xc400)==0x5a;
}
bool sg1000Rom() {
    struct Layout { size_t size; std::array<uint8_t, 6> pages; };
    constexpr Layout layouts[]{
        {8192, {1, 1, 1, 1, 0xff, 0xff}}, {16384, {1, 2, 1, 2, 0xff, 0xff}},
        {32768, {1, 2, 3, 4, 0xff, 0xff}}, {40960, {1, 2, 3, 4, 5, 5}},
        {49152, {1, 2, 3, 4, 5, 6}},
    };
    for (const auto& [size, pages] : layouts) {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; ++i)
            bytes[i] = static_cast<uint8_t>(i / 8192 + ((i & 8191) == 8191 ? 0x11 : 1));
        const auto image = smsgg::RomLoader::fromBytes(std::move(bytes),
            {.mapper = smsgg::MapperKind::RomOnly, .machine = smsgg::Machine::Sg1000});
        const auto* loaded = std::get_if<smsgg::RomImage>(&image);
        if (!loaded) return false;
        smsgg::Cartridge cartridge(*loaded);
        MemoryBus bus(cartridge, smsgg::Machine::Sg1000);
        for (size_t i = 0; i < pages.size(); ++i) {
            const auto first = static_cast<uint16_t>(i * 8192);
            const auto last = static_cast<uint16_t>(first + 8191);
            const auto page = pages[i];
            bus.write8(first, 0x56);
            if (bus.read8(first) != page || bus.read8(last) != (page == 0xff ? 0xff : page + 0x10))
                return false;
        }
    }
    return true;
}

bool enable() {
    Memory external;
    MemoryBus bus(external);
    for (unsigned control = 0; control < 256; ++control) {
        const bool ram = control / 16 % 2 == 0, cartridge = control / 64 % 2 == 0;
        for (const uint16_t address : {0, 0x3ff, 0x400, 0x3fff, 0x4000, 0x7fff, 0x8000,
                0xbfff, 0xc000, 0xdfff, 0xe000, 0xfffc, 0xfffd, 0xfffe, 0xffff}) {
            bus.setMemoryControl(0);
            bus.write8(address, 0x35); external.bytes[address] = 0xa6;
            bus.setMemoryControl(static_cast<uint8_t>(control)); external.accesses.clear();
            const uint8_t expected = address >= 0xc000 ? (ram ? 0x35 : 0xff) : (cartridge ? 0xa6 : 0xff);
            if (bus.read8(address) != expected) return false;
            if (address < 0xc000 && cartridge) {
                if (!external.accessedOnce(false, address, 0xa6)) return false;
            } else if (!external.accesses.empty()) return false;
            external.accesses.clear(); bus.write8(address, 0x79);
            if (cartridge) {
                if (!external.accessedOnce(true, address, 0x79)) return false;
            } else if (!external.accesses.empty()) return false;
            bus.setMemoryControl(0);
            if (address >= 0xc000) {
                if (bus.read8(address) != (ram ? 0x79 : 0x35)
                    || bus.read8(static_cast<uint16_t>(address ^ 0x2000)) != (ram ? 0x79 : 0x35)) return false;
            } else if (external.bytes[address] != (cartridge ? 0x79 : 0xa6)) return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "initial") return initial() ? 0 : 1;
        if (name == "mirrors") return mirrors() ? 0 : 1;
        if (name == "boundaries") return boundaries() ? 0 : 1;
        if (name == "external_read") return externalRead() ? 0 : 1;
        if (name == "external_write") return externalWrite() ? 0 : 1;
        if (name == "overlap") return overlap() ? 0 : 1;
        if (name == "enable") return enable() ? 0 : 1;
        if (name == "sg1000_map") return sg1000Map() ? 0 : 1;
        if (name == "sg1000_fetch") return sg1000Fetch() ? 0 : 1;
        if (name == "sg1000_rom") return sg1000Rom() ? 0 : 1;
    }
    std::cerr << "Failed MemoryBus test\n";
    return 1;
}
