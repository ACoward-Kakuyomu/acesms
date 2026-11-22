#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace smsgg::test {
// Original, BIOS-free fixture. C000=IRQ count, C001=NMI count, C002=pad, C003=status.
inline std::vector<uint8_t> stage5InputRom(bool countFrames = false) {
    std::vector<uint8_t> rom(8192);
    size_t offset = 0;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (const auto byte : bytes) rom[offset++] = byte;
    };
    auto out = [&](uint8_t port, uint8_t value) { emit({0x3e, value, 0xd3, port}); };
    auto command = [&](uint16_t address, uint8_t code) {
        out(0xbf, static_cast<uint8_t>(address));
        out(0xbf, static_cast<uint8_t>((address >> 8) | (code << 6)));
    };
    auto reg = [&](uint8_t index, uint8_t value) { out(0xbf, value); out(0xbf, 0x80 | index); };
    emit({0xc3, 0x00, 0x01});
    offset = 0x38;
    emit({0xf5, 0xe5, 0xdb, 0xbf, 0x32, 0x03, 0xc0, 0x21, 0x00, 0xc0,
        0x34, 0xe1, 0xf1, 0xfb, 0xed, 0x4d});
    offset = 0x66;
    emit({0xf5, 0xe5, 0x21, 0x01, 0xc0, 0x34, 0xe1, 0xf1, 0xed, 0x45});
    offset = 0x100;
    emit({0xf3, 0x31, 0xf8, 0xdf});
    reg(0, 4); reg(2, 0x0e); reg(5, 0x7e);
    command(0, 1);
    for (unsigned row = 0; row < 8; ++row) {
        out(0xbe, row % 2 ? 0x55 : 0xaa);
        out(0xbe, row % 2 ? 0xaa : 0x55);
        out(0xbe, 0); out(0xbe, 0);
    }
    command(0, 3); out(0xbe, 0); out(0xbe, 3); out(0xbe, 0x30);
    command(0x3f00, 1); out(0xbe, 0xd0); // End of sprite list.
    reg(1, 0x60);
    emit({0xed, 0x56, 0xfb}); // IM 1, EI.
    const auto loop = static_cast<uint16_t>(offset);
    emit({0x76, 0xdb, 0xdc, 0x32, 0x02, 0xc0, 0xe6, 0x3f, 0xfe, 0x3f,
        0x3e, 0x03, 0x28, 0x02, 0x3e, 0x0c, 0x47});
    if (countFrames) emit({0x3a, 0x00, 0xc0, 0xe6, 0x03, 0x47});
    else emit({0x3a, 0x01, 0xc0, 0xe6, 0x01, 0x28, 0x02, 0x06, 0x0f}); // Odd NMI count: yellow.
    command(1, 3);
    emit({0x78, 0xd3, 0xbe, 0xc3, static_cast<uint8_t>(loop), static_cast<uint8_t>(loop >> 8)});
    return rom;
}

inline std::vector<uint8_t> stage5PrefixRom() {
    // Fill RAM with DD, then replace the executing ROM bank with all-DD data.
    std::vector<uint8_t> rom(65536, 0xdd);
    size_t at = 0x400;
    for (const uint8_t value : {0xf3, 0x3e, 0xdd, 0x32, 0x00, 0xc0, 0x21, 0x00, 0xc0,
        0x11, 0x01, 0xc0, 0x01, 0xff, 0x1f, 0xed, 0xb0, 0x32, 0xfd, 0xff}) rom[at++] = value;
    return rom;
}
} // namespace smsgg::test
