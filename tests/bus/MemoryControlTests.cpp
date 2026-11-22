#include "core/bus/MemoryControl.h"
#include "core/bus/IoBus.h"
#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"
#include "IoTestSupport.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

namespace {
bool aliases() {
    smsgg::test::Memory external;
    smsgg::MemoryBus bus(external);
    smsgg::test::Io devices;
    smsgg::MemoryControl control(bus, devices);
    smsgg::IoBus io(control);
    for (unsigned high = 0; high < 256; ++high) {
        for (unsigned low = 0; low < 64; low += 2) {
            for (const uint8_t value : {0, 0x04, 0x10, 0x40, 0x50}) {
                bus.setMemoryControl(0); bus.write8(0xc000, 0x56); external.bytes[0x4000] = 0xa3;
                external.accesses.clear(); devices.accesses.clear();
                io.writePort(static_cast<uint16_t>(high * 256 + low), value);
                if (!devices.accesses.empty() || !external.accesses.empty()) return false;
                if (bus.read8(0xc000) != (value == 0x10 || value == 0x50 ? 0xff : 0x56)
                    || bus.read8(0x4000) != (value == 0x40 || value == 0x50 ? 0xff : 0xa3)) return false;
                if (external.accesses.size() != (value == 0x40 || value == 0x50 ? 0 : 1)) return false;
                devices.reply = 0x5a;
                if (io.readPort(0xdc) != (value == 4 ? 0xff : 0x5a)) return false;
            }
        }
    }
    return true;
}
bool delegation() {
    smsgg::test::Memory external;
    smsgg::MemoryBus bus(external);
    smsgg::test::Io devices;
    smsgg::MemoryControl control(bus, devices);
    smsgg::IoBus io(control);
    for (unsigned port = 0; port < 65536; ++port) {
        const unsigned low = port % 256;
        const uint16_t read = static_cast<uint16_t>((low < 64 ? 0x3e : low < 128 ? 0x7e : low < 192 ? 0xbe : 0xdc) + low % 2);
        const int write = low < 64 ? (low % 2 == 0 ? -1 : 0x3f) : low < 128 ? 0x7f
            : low < 192 ? (low % 2 == 0 ? 0xbe : 0xbf) : -1;
        for (const uint8_t value : {0, 0x55, 0xaa, 0xff}) {
            io.writePort(0x3e, 0); // Each delegation case starts with controller I/O enabled.
            devices.reply = value; devices.accesses.clear();
            io.writePort(static_cast<uint16_t>(port), value);
            if (devices.accesses.size() != (write < 0 ? 0 : 1)) return false;
            if (write >= 0 && (!devices.accesses[0].write || devices.accesses[0].port != write || devices.accesses[0].value != value)) return false;
            devices.accesses.clear();
            if (io.readPort(static_cast<uint16_t>(port)) != value || devices.accesses.size() != 1
                || devices.accesses[0].write || devices.accesses[0].port != read || devices.accesses[0].value != value) return false;
        }
    }
    return true;
}
bool ioDisable() {
    for (const auto machine : {smsgg::Machine::Sms, smsgg::Machine::GameGear}) {
        smsgg::test::Memory external;
        smsgg::MemoryBus bus(external);
        smsgg::test::Io devices;
        smsgg::MemoryControl control(bus, devices, machine);
        devices.reply = 0x5a;
        bus.write8(0xc000, 0x76);
        external.bytes[0x4000] = 0x42;
        for (const uint8_t value : {0x04, 0x00, 0x04, 0x00}) {
            control.writePort(0x3e, value);
            const bool disabled = machine == smsgg::Machine::Sms && value == 4;
            for (const uint16_t port : {0xdc, 0xdd}) {
                devices.accesses.clear();
                if (control.readPort(port) != (disabled ? 0xff : 0x5a)
                    || devices.accesses.size() != (disabled ? 0u : 1u)) return false;
            }
            devices.accesses.clear(); control.writePort(0x3f, 0xaa);
            if (devices.accesses.size() != (disabled ? 0u : 1u)) return false;
            if (bus.read8(0xc000) != 0x76 || bus.read8(0x4000) != 0x42) return false;
            for (const uint16_t port : {0x7e, 0xbe, 0xf2}) {
                devices.accesses.clear();
                if (control.readPort(port) != 0x5a || devices.accesses.size() != 1) return false;
            }
            for (const uint16_t port : {0x7f, 0xbf, 0xf0, 0xf1, 0xf2}) {
                devices.accesses.clear(); control.writePort(port, 0x12);
                if (devices.accesses.size() != 1 || devices.accesses[0].port != port) return false;
            }
        }
    }
    return true;
}
bool cpu() {
    std::vector<uint8_t> bytes(65536);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(0x20 + i / 16384);
    smsgg::Cartridge cartridge(std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(bytes), {smsgg::MapperKind::Sega})));
    smsgg::MemoryBus bus(cartridge);
    smsgg::test::Io devices;
    smsgg::MemoryControl control(bus, devices);
    smsgg::IoBus io(control);
    for (const uint8_t value : {0, 0x10, 0x40, 0x50}) {
        bus.setMemoryControl(0); bus.write8(0xffff, 2);
        auto expected = smsgg::test::initialRegisters(); expected.setA(value); expected.pc = 0xfffe; expected.r = 0xff;
        smsgg::Z80 z80(expected);
        const std::array<uint8_t, 2> instruction{0xd3, 0x3e};
        const auto time = z80.executeInstruction(instruction, &bus, &io);
        expected.pc = 0; expected.r = 0x80;
        if (!time || *time != 11 || !smsgg::test::sameRegisters(z80.registers(), expected) || !devices.accesses.empty()) return false;
        bus.write8(0xffff, 3);
        if (bus.read8(0xffff) != (value == 0x10 || value == 0x50 ? 0xff : 3)) return false;
        if (bus.read8(0x8000) != (value == 0x40 || value == 0x50 ? 0xff : 0x23)) return false;
        bus.setMemoryControl(0);
        if (bus.read8(0x8000) != (value == 0x40 || value == 0x50 ? 0x22 : 0x23)
            || bus.read8(0xffff) != (value == 0x10 || value == 0x50 ? 2 : 3)) return false;
    }
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "aliases") return aliases() ? 0 : 1;
        if (name == "delegation") return delegation() ? 0 : 1;
        if (name == "cpu") return cpu() ? 0 : 1;
        if (name == "io_disable") return ioDisable() ? 0 : 1;
    }
    std::cerr << "Failed MemoryControl test\n";
    return 1;
}
