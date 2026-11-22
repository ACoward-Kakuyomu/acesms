#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace smsgg::test {
inline std::vector<uint8_t> sg1000TestRom() {
    std::vector<uint8_t> bytes(8192);
    size_t at = 0;
    const auto emit = [&](std::initializer_list<uint8_t> code) {
        for (const auto byte : code) bytes[at++] = byte;
    };
    const auto patch = [&](size_t instruction, size_t address) {
        bytes[instruction + 1] = static_cast<uint8_t>(address);
        bytes[instruction + 2] = static_cast<uint8_t>(address >> 8);
    };
    const auto jump = [&](size_t address) {
        emit({0xc3, static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8)});
    };
    const auto writePort = [&](uint8_t port, uint8_t value) {
        emit({0x3e, value, 0xd3, port}); // LD A,value; OUT (port),A
    };
    const auto writeReg = [&](uint8_t reg, uint8_t value) {
        writePort(0xbf, value);
        writePort(0xbf, static_cast<uint8_t>(0x80 | reg));
    };
    const auto writeVram = [&](uint16_t address, std::initializer_list<uint8_t> data) {
        writePort(0xbf, static_cast<uint8_t>(address));
        writePort(0xbf, static_cast<uint8_t>(0x40 | (address >> 8)));
        for (const auto byte : data) writePort(0xbe, byte);
    };
    emit({0xc3, 0x00, 0x01}); // JP $0100
    at = 0x38;
    emit({0xf5, 0xe5, 0xdb, 0xbf, 0x32, 0x02, 0xc0, // Read VDP status into $C002.
        0x21, 0x01, 0xc0, 0x34, 0xe1, 0xf1, 0xfb, 0xed, 0x4d}); // Increment $C001; EI; RETI.
    at = 0x66;
    emit({0xf5, 0x3a, 0x03, 0xc0, 0xee, 0x01, 0x32, 0x03, 0xc0, 0xf1, 0xed, 0x45}); // Toggle $C003; RETN.
    at = 0x100;
    emit({0xf3, 0x31, 0x00, 0xc4, 0xed, 0x56}); // DI; LD SP,$C400; IM 1.
    writeReg(1, 0x80);
    writeReg(2, 14); writeReg(3, 0xff); writeReg(5, 0x20); writeReg(6, 4);
    writeVram(0, {0x80});
    writeVram(0x2000, {0x40}); // Graphics II color and sprite pattern.
    writeVram(0x3fc0, {0xf0}); // Graphics I color.
    writeVram(0x1000, {0xff, 24, 0, 15, 0xff, 24, 0, 15, 0xff, 40, 0, 15,
        0xff, 48, 0, 15, 0xff, 56, 0, 15, 0xd0});
    writeReg(7, 0xf2); // White text, green background while idle.
    writeReg(1, 0xe0);
    writePort(0x7f, 0x80); writePort(0x7f, 0); writePort(0x7f, 0x90); // Tone 0, audible.
    emit({0xfb});
    const auto loop = at;
    emit({0x76, 0xdb, 0xc0, 0x32, 0, 0xc0}); // HALT; poll 1P.
    writePort(0x3f,0); // SG-1000 pad inputs are independent of the SMS direction latch.
    emit({0xdb,0xfe,0x32,4,0xc0,0xdb,0xc1,0x32,5,0xc0,0xdb,0xff,0x32,6,0xc0});
    emit({0x3a,0,0xc0});
    emit({0xe6, 3, 0x5f, 0x16, 0, 0x21, 0, 3, 0x19, 0x7e, 0xd3, 0xbf}); // R0 table indexed by direction bits.
    writePort(0xbf, 0x80);
    emit({0x21, 4, 3, 0x19, 0x7e, 0xd3, 0xbf}); // Read R1 from the next table.
    writePort(0xbf, 0x81);
    emit({0x3a, 3, 0xc0, 0xb7}); // Read Hold bit.
    const auto ifHold = at;
    emit({0xc2, 0, 0});
    writePort(0x7f, 0x90);
    emit({0x3a, 0, 0xc0, 0xe6, 0x10}); // Test active-low button 1.
    const auto ifPressed = at;
    emit({0xca, 0, 0});
    writeReg(7, 0xf2);
    jump(loop);
    patch(ifPressed, at);
    writeReg(7, 0xf8); // Red when button 1 is pressed.
    jump(loop);
    patch(ifHold, at);
    writeReg(7, 0xf4); // Blue and silent during Hold.
    writePort(0x7f, 0x9f);
    jump(loop);
    at = 0x300;
    emit({0, 0, 2, 0, 0xe8, 0xf0, 0xe0, 0xe0}); // Both, down, up, neutral.
    return bytes;
}
} // namespace smsgg::test
