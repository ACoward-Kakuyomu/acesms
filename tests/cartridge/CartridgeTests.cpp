#include "core/cartridge/RomOnly.h"
#include "core/cartridge/RomLoader.h"
#include "core/cartridge/Cartridge.h"
#include "core/bus/MemoryBus.h"

#include <iostream>
#include <string_view>

namespace {
bool romOnly() {
    for (const size_t size : {8192, 16384, 32768, 49152}) {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(i ^ (i / 256));
        auto loaded = smsgg::RomLoader::fromBytes(bytes, {smsgg::MapperKind::RomOnly});
        const auto* image = std::get_if<smsgg::RomImage>(&loaded);
        if (!image) return false;
        smsgg::RomOnly mapper(*image);
        for (const uint8_t value : {0, 0x55, 0xaa, 0xff}) {
            size_t position = 0;
            for (unsigned address = 0; address < 65536; ++address) {
                const auto a = static_cast<uint16_t>(address);
                const uint8_t expected = address < 0xc000 ? bytes[position] : 0xff;
                if (mapper.read8(a) != expected) return false;
                mapper.write8(a, value);
                if (mapper.read8(a) != expected) return false;
                if (++position == size) position = 0;
            }
            mapper.reset();
            if (mapper.read8(0) != bytes[0] || mapper.read8(0xbfff) != bytes[49151 % size]) return false;
        }
    }
    return true;
}
smsgg::Cartridge makeCartridge(smsgg::MapperKind kind, size_t sram, uint8_t pattern) {
    std::vector<uint8_t> bytes(kind == smsgg::MapperKind::Sega ? 65536 : 49152);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(pattern + i / 16384);
    return smsgg::Cartridge(std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(bytes), {kind, sram})));
}
bool ownership() {
    for (const auto kind : {smsgg::MapperKind::RomOnly, smsgg::MapperKind::Sega}) {
        for (const size_t capacity : {0, 8192, 16384, 32768}) {
            if (kind == smsgg::MapperKind::RomOnly && capacity != 0) continue;
            auto first = makeCartridge(kind, capacity, 0x20);
            auto second = makeCartridge(kind, capacity, 0xa0);
            smsgg::MemoryBus bus(first);
            for (unsigned address = 0; address < 49152; ++address) {
                const auto a = static_cast<uint16_t>(address);
                if (bus.read8(a) != 0x20 + address / 16384 || second.read8(a) != 0xa0 + address / 16384) return false;
            }
            bus.write8(0xffff, 3);
            if (bus.read8(0x8000) != (kind == smsgg::MapperKind::Sega ? 0x23 : 0x22)) return false;
            if (bus.read8(0xdfff) != 3 || second.read8(0x8000) != 0xa2) return false;
            bus.write8(0xc000, 0x59);
            bus.write8(0xfffc, 8);
            if (kind == smsgg::MapperKind::Sega) {
                if (first.read8(0x8000) != (capacity == 0 ? 0xff : 0)) return false;
                bus.write8(0x8000, 0x67);
                if (bus.read8(0x8000) != (capacity == 0 ? 0xff : 0x67)) return false;
                second.write8(0xfffc, 8);
                if (second.read8(0x8000) != (capacity == 0 ? 0xff : 0)) return false;
            }
            first.reset();
            if (bus.read8(0x8000) != 0x22 || bus.read8(0xe000) != 0x59) return false;
            bus.write8(0xfffc, 8);
            if (kind == smsgg::MapperKind::Sega && bus.read8(0x8000) != (capacity == 0 ? 0xff : 0x67)) return false;
        }
    }
    return true;
}
bool busRegisters() {
    auto cartridge = makeCartridge(smsgg::MapperKind::Sega, 32768, 0x20);
    smsgg::MemoryBus bus(cartridge);
    for (unsigned slot = 0; slot < 3; ++slot) {
        const auto high = static_cast<uint16_t>(0xfffd + slot);
        const auto low = static_cast<uint16_t>(0xdffd + slot);
        const auto sample = static_cast<uint16_t>(slot * 16384 + 0x400);
        for (const uint8_t bank : {0, 3, 0xff, 1, 1}) {
            bus.write8(high, bank);
            if (bus.read8(sample) != 0x20 + bank % 4 || bus.read8(high) != bank || bus.read8(low) != bank) return false;
            bus.write8(low, 0x82);
            if (bus.read8(sample) != 0x20 + bank % 4 || bus.read8(high) != 0x82) return false;
            if (bus.read8(0) != 0x20 || bus.read8(0x3ff) != 0x20) return false;
        }
    }
    for (const uint8_t control : {8, 12}) {
        bus.write8(0xfffc, control);
        bus.write8(0x8000, static_cast<uint8_t>(control + 0x50));
        bus.write8(0xdffc, 0);
        if (bus.read8(0xfffc) != 0 || bus.read8(0x8000) != control + 0x50) return false;
    }
    bus.write8(0xfffc, 8);
    if (bus.read8(0x8000) != 0x58) return false;
    bus.write8(0xffff, 3); bus.write8(0xfffc, 0);
    return bus.read8(0x8000) == 0x23 && bus.read8(0xdfff) == 3;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "rom_only") return romOnly() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "ownership") return ownership() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "bus_registers") return busRegisters() ? 0 : 1;
    std::cerr << "Failed cartridge test\n";
    return 1;
}
