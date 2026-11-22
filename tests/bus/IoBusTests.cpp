#include "core/bus/IoBus.h"
#include "IoTestSupport.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

using smsgg::IoBus;
using smsgg::test::Io;

namespace {
bool read() {
    Io devices;
    IoBus bus(devices);
    struct Range { unsigned first, last; uint16_t even, odd; };
    constexpr std::array<Range, 4> ranges{{
        {0, 63, 0x3e, 0x3f}, {64, 127, 0x7e, 0x7f},
        {128, 191, 0xbe, 0xbf}, {192, 255, 0xdc, 0xdd},
    }};
    for (unsigned high = 0; high < 256; ++high) {
        for (const auto& range : ranges) {
            for (unsigned low = range.first; low <= range.last; ++low) {
                for (const uint8_t value : {0, 0x55, 0xaa, 0xff}) {
                    const auto port = static_cast<uint16_t>(high * 256 + low);
                    const auto expected = low % 2 == 0 ? range.even : range.odd;
                    devices.reply = value; devices.accesses.clear();
                    if (bus.readPort(port) != value || devices.accesses.size() != 1
                        || devices.accesses[0].write || devices.accesses[0].port != expected
                        || devices.accesses[0].value != value) {
                        std::cerr << "read port=" << port << " reply=" << unsigned(value) << '\n';
                        return false;
                    }
                }
            }
        }
    }
    return true;
}
bool write() {
    Io devices;
    IoBus bus(devices);
    struct Range { unsigned first, last; uint16_t even, odd; };
    constexpr std::array<Range, 4> ranges{{
        {0, 63, 0x3e, 0x3f}, {64, 127, 0x7f, 0x7f},
        {128, 191, 0xbe, 0xbf}, {192, 255, 0x100, 0x100},
    }};
    for (unsigned high = 0; high < 256; ++high) {
        for (const auto& range : ranges) {
            for (unsigned low = range.first; low <= range.last; ++low) {
                for (const uint8_t value : {0, 0x55, 0xaa, 0xff}) {
                    const auto port = static_cast<uint16_t>(high * 256 + low);
                    const auto expected = low % 2 == 0 ? range.even : range.odd;
                    devices.accesses.clear(); bus.writePort(port, value);
                    if (expected == 0x100) {
                        if (!devices.accesses.empty()) return false;
                    } else if (devices.accesses.size() != 1 || !devices.accesses[0].write
                        || devices.accesses[0].port != expected || devices.accesses[0].value != value) {
                        std::cerr << "write port=" << port << " value=" << unsigned(value) << '\n';
                        return false;
                    }
                }
            }
        }
    }
    // Control writes do not turn unconnected reads into latch readback.
    for (const uint16_t port : {0x3e, 0x3f}) {
        devices.reply = 0xa5; devices.accesses.clear(); bus.writePort(port, 0x42);
        if (bus.readPort(port) != 0xa5 || devices.accesses.size() != 2
            || !devices.accesses[0].write || devices.accesses[1].write
            || devices.accesses[1].port != port) return false;
    }
    return true;
}
bool sg1000() {
    Io devices;
    devices.reply = 0x5a;
    IoBus bus(devices, smsgg::Machine::Sg1000, true);
    constexpr int unused = -1;
    constexpr std::array<int, 8> reads{unused, unused, unused, unused, 0xbe, 0xbf, 0xdc, 0xdd};
    constexpr std::array<int, 8> writes{unused, unused, 0x7f, 0x7f, 0xbe, 0xbf, unused, unused};
    for (unsigned low = 0; low < 256; ++low) {
        const auto port = static_cast<uint16_t>(0xa500 | low);
        const auto index = ((low >> 6) << 1) | (low & 1);
        devices.accesses.clear();
        if (bus.readPort(port) != (reads[index] == unused ? 0xff : 0x5a)) return false;
        if (reads[index] == unused ? !devices.accesses.empty()
            : devices.accesses.size() != 1 || devices.accesses[0].write
                || devices.accesses[0].port != reads[index]) return false;
        devices.accesses.clear();
        bus.writePort(port, 0x93);
        if (writes[index] == unused ? !devices.accesses.empty()
            : devices.accesses.size() != 1 || !devices.accesses[0].write
                || devices.accesses[0].port != writes[index] || devices.accesses[0].value != 0x93) return false;
    }

    smsgg::test::Memory rom;
    constexpr uint8_t program[]{
        0x3e, 0x91, // LD A,$91
        0xd3, 0x40, // OUT ($40),A
        0xd3, 0x81, // OUT ($81),A
        0xdb, 0xde, // IN A,($DE)
    };
    std::ranges::copy(program, rom.bytes.begin());
    smsgg::Z80 cpu(smsgg::Z80Registers{});
    devices.accesses.clear();
    for (const uint32_t cycles : {7, 11, 11, 11}) {
        const auto result = cpu.step(rom, bus);
        const auto* elapsed = std::get_if<uint32_t>(&result);
        if (!elapsed || *elapsed != cycles) return false;
    }
    const auto& accesses = devices.accesses;
    return cpu.registers().a() == 0x5a && accesses.size() == 3
        && accesses[0].write && accesses[0].port == 0x7f && accesses[0].value == 0x91
        && accesses[1].write && accesses[1].port == 0xbf && accesses[1].value == 0x91
        && !accesses[2].write && accesses[2].port == 0xdc;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "read") return read() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "write") return write() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "sg1000") return sg1000() ? 0 : 1;
    std::cerr << "Failed IoBus test\n";
    return 1;
}
