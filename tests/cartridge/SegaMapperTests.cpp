#include "core/cartridge/SegaMapper.h"
#include "core/cartridge/RomLoader.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {
smsgg::RomImage makeImage(size_t size, size_t sram = 0) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<uint8_t>((i / 16384) * 37 + (i % 16384) / 256 * 3 + i % 256);
    }
    return std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(bytes), {smsgg::MapperKind::Sega, sram}));
}

uint8_t expectedByte(const smsgg::RomImage& image, unsigned bank, size_t offset) {
    const size_t pages = (image.bytes().size() + 16383) / 16384;
    if (offset >= image.bytes().size()) offset -= image.bytes().size(); // 8KiB within a 16KiB slot.
    return image.bytes()[(bank % pages) * 16384 + offset];
}

bool rom() {
    constexpr std::array<unsigned, 7> boundaries{0, 0x3ff, 0x400, 0x1fff, 0x2000, 0x3ffe, 0x3fff};
    constexpr std::array<unsigned, 8> probes{0, 0x3ff, 0x400, 0x3fff, 0x4000, 0x7fff, 0x8000, 0xbfff};
    for (const size_t size : {8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576}) {
        const auto image = makeImage(size);
        smsgg::SegaMapper mapper(image);
        for (unsigned slot = 0; slot < 3; ++slot) {
            for (unsigned bank = 0; bank < 256; ++bank) {
                mapper.reset(); mapper.write8(static_cast<uint16_t>(0xfffd + slot), static_cast<uint8_t>(bank));
                for (const auto offset : boundaries) {
                    const auto address = static_cast<uint16_t>(slot * 16384 + offset);
                    const auto expected = slot == 0 && offset < 1024 ? image.bytes()[offset] : expectedByte(image, bank, offset);
                    if (mapper.read8(address) != expected) return false;
                }
                for (const auto address : probes) {
                    const unsigned region = address / 16384;
                    const auto expected = address < 1024 ? image.bytes()[address]
                        : expectedByte(image, region == slot ? bank : region, address % 16384);
                    if (mapper.read8(static_cast<uint16_t>(address)) != expected) return false;
                }
                if (bank == 0 || bank == 255) {
                    for (unsigned offset = 0; offset < 16384; ++offset) {
                        const auto expected = slot == 0 && offset < 1024 ? image.bytes()[offset] : expectedByte(image, bank, offset);
                        if (mapper.read8(static_cast<uint16_t>(slot * 16384 + offset)) != expected) return false;
                    }
                }
            }
        }
    }
    return true;
}

bool reset() {
    for (const size_t size : {8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576}) {
        const auto image = makeImage(size);
        smsgg::SegaMapper mapper(image);
        for (unsigned address = 0; address < 0xc000; ++address) {
            const auto expected = expectedByte(image, address / 16384, address % 16384);
            mapper.write8(static_cast<uint16_t>(address), static_cast<uint8_t>(address));
            if (mapper.read8(static_cast<uint16_t>(address)) != expected) return false;
        }
        for (unsigned slot = 0; slot < 3; ++slot) {
            mapper.write8(static_cast<uint16_t>(0xdffd + slot), 0xff);
            if (mapper.read8(static_cast<uint16_t>(slot * 16384 + 1024)) != expectedByte(image, slot, 1024)) return false;
            mapper.write8(static_cast<uint16_t>(0xfffd + slot), 0xff);
        }
        mapper.reset();
        for (unsigned address = 0; address < 65536; ++address) {
            const auto expected = address < 0xc000 ? expectedByte(image, address / 16384, address % 16384) : 0xff;
            if (mapper.read8(static_cast<uint16_t>(address)) != expected) return false;
        }
    }
    return true;
}
bool sram() {
    for (const size_t size : {0, 8192, 16384, 32768}) {
        const auto image = makeImage(65536, size);
        std::vector<uint8_t> storage(size), expected(size);
        smsgg::SegaMapper mapper(image, storage);
        for (unsigned offset = 0; offset < 16384; ++offset) {
            mapper.write8(static_cast<uint16_t>(0x8000 + offset), 0xff);
            if (mapper.read8(static_cast<uint16_t>(0x8000 + offset)) != expectedByte(image, 2, offset)) return false;
        }
        for (unsigned bank = 0; bank < 2; ++bank) {
            mapper.write8(0xfffc, static_cast<uint8_t>(8 + bank * 4));
            for (unsigned offset = 0; offset < 16384; ++offset) {
                const auto index = size == 0 ? 0 : (bank * 16384 + offset) % size;
                if (mapper.read8(static_cast<uint16_t>(0x8000 + offset)) != (size == 0 ? 0xff : expected[index])) return false;
                const auto value = static_cast<uint8_t>(offset ^ (offset / 256) * 3 ^ (bank * 0x55));
                mapper.write8(static_cast<uint16_t>(0x8000 + offset), value);
                if (size != 0) expected[index] = value;
            }
            for (unsigned checkBank = 0; checkBank < 2; ++checkBank) {
                mapper.write8(0xfffc, static_cast<uint8_t>(8 + checkBank * 4));
                for (unsigned offset = 0; offset < 16384; ++offset) {
                    const auto value = size == 0 ? 0xff : expected[(checkBank * 16384 + offset) % size];
                    if (mapper.read8(static_cast<uint16_t>(0x8000 + offset)) != value) return false;
                }
            }
        }
        mapper.write8(0xffff, 0xff); // Keep updating ROM bank while SRAM is selected.
        for (unsigned control = 0; control < 256; ++control) {
            mapper.write8(0xfffc, static_cast<uint8_t>(control));
            for (const unsigned offset : {0, 0x1fff, 0x2000, 0x3fff}) {
                const auto value = (control & 8) == 0 ? expectedByte(image, 255, offset)
                    : size == 0 ? 0xff : expected[((control / 4 % 2) * 16384 + offset) % size];
                if (mapper.read8(static_cast<uint16_t>(0x8000 + offset)) != value) return false;
            }
            if (mapper.read8(0xc000) != 0xff || mapper.read8(0) != image.bytes()[0]) return false;
        }
        mapper.write8(0xfffc, 0); mapper.write8(0x8000, 0x42);
        mapper.reset();
        if (mapper.read8(0x8000) != expectedByte(image, 2, 0)) return false;
        for (unsigned bank = 0; bank < 2; ++bank) {
            mapper.write8(0xfffc, static_cast<uint8_t>(8 + bank * 4));
            for (unsigned offset = 0; offset < 16384; ++offset) {
                const auto value = size == 0 ? 0xff : expected[(bank * 16384 + offset) % size];
                if (mapper.read8(static_cast<uint16_t>(0x8000 + offset)) != value) return false;
            }
        }
    }
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "rom") return rom() ? 0 : 1;
        if (name == "reset") return reset() ? 0 : 1;
        if (name == "sram") return sram() ? 0 : 1;
    }
    std::cerr << "Failed Sega Mapper test\n";
    return 1;
}
