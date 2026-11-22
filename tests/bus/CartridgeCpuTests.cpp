#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"
#include "BusTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::run;
using smsgg::test::TraceMemory;

namespace {
smsgg::Cartridge makeCartridge() {
    std::vector<uint8_t> bytes(65536);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(0x20 + i / 16384);
    return smsgg::Cartridge(std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(bytes), {smsgg::MapperKind::Sega, 32768})));
}
bool loads() {
    auto cartridge = makeCartridge();
    smsgg::MemoryBus bus(cartridge);
    TraceMemory trace(bus);
    bus.write8(0xfffd, 3);
    struct Sample { uint16_t address; uint8_t rom; };
    constexpr Sample samples[]{{0, 0x20}, {0x3ff, 0x20}, {0x400, 0x23}, {0x3fff, 0x23},
        {0x4000, 0x21}, {0x7fff, 0x21}, {0x8000, 0x22}, {0xbfff, 0x22}};
    for (const uint8_t control : {0, 8, 12}) {
        bus.write8(0xfffc, control);
        bus.write8(0x8000, static_cast<uint8_t>(control + 0x60));
        bus.write8(0xbfff, static_cast<uint8_t>(control + 0x61));
        for (const auto sample : samples) {
            auto expected = initialRegisters(); expected.pc = 0xffff; expected.r = 0xff;
            smsgg::Z80 cpu(expected); trace.accesses.clear();
            const auto time = run(cpu, {0x3a, static_cast<uint8_t>(sample.address), static_cast<uint8_t>(sample.address >> 8)}, &trace);
            const uint8_t value = control != 0 && sample.address >= 0x8000
                ? static_cast<uint8_t>(control + (sample.address == 0x8000 ? 0x60 : 0x61)) : sample.rom;
            expected.setA(value); expected.pc = 2; expected.r = 0x80;
            if (!time || *time != 13 || !sameRegisters(cpu.registers(), expected) || trace.accesses.size() != 1) return false;
            const auto access = trace.accesses[0];
            if (access.write || access.address != sample.address || access.value != value) return false;
        }
    }
    return true;
}
bool registerWrites() {
    auto cartridge = makeCartridge();
    smsgg::MemoryBus bus(cartridge);
    TraceMemory trace(bus);
    struct Register { uint16_t address, sample; uint8_t value, before, after; };
    constexpr Register registers[]{{0xfffc, 0x8000, 8, 0x22, 0}, {0xfffd, 0x400, 3, 0x20, 0x23},
        {0xfffe, 0x4000, 3, 0x21, 0x23}, {0xffff, 0x8000, 3, 0x22, 0x23}};
    for (const auto item : registers) {
        for (const bool mirror : {false, true}) {
            cartridge.reset();
            const auto address = static_cast<uint16_t>(item.address - (mirror ? 0x2000 : 0));
            auto expected = initialRegisters(); expected.setA(item.value); expected.pc = 0xffff; expected.r = 0xff;
            smsgg::Z80 cpu(expected); trace.accesses.clear();
            const auto time = run(cpu, {0x32, static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8)}, &trace);
            expected.pc = 2; expected.r = 0x80;
            if (!time || *time != 13 || !sameRegisters(cpu.registers(), expected)
                || bus.read8(item.sample) != (mirror ? item.before : item.after)
                || bus.read8(item.address) != item.value || trace.accesses.size() != 1) return false;
            const auto access = trace.accesses[0];
            if (!access.write || access.address != address || access.value != item.value) return false;
        }
    }
    return true;
}
bool stack() {
    auto cartridge = makeCartridge();
    smsgg::MemoryBus bus(cartridge);
    TraceMemory trace(bus);
    for (const uint16_t sp : {0xc002, 0xe002, 0x8002, 0}) {
        cartridge.reset(); bus.write8(0xfffc, 8);
        auto expected = initialRegisters(); expected.sp = sp; expected.pc = 0xffff; expected.r = 0xff;
        smsgg::Z80 cpu(expected); trace.accesses.clear();
        const auto push = run(cpu, {0xc5}, &trace), pop = run(cpu, {0xd1}, &trace);
        expected.de = expected.bc; expected.pc = 1; expected.r = 0x81;
        if (!push || *push != 11 || !pop || *pop != 10 || !sameRegisters(cpu.registers(), expected) || trace.accesses.size() != 4) return false;
        const std::array<smsgg::test::Memory::Access, 4> wanted{{
            {true, static_cast<uint16_t>(sp - 1), 0x23}, {true, static_cast<uint16_t>(sp - 2), 0x45},
            {false, static_cast<uint16_t>(sp - 2), 0x45}, {false, static_cast<uint16_t>(sp - 1), 0x23}}};
        for (size_t i = 0; i < wanted.size(); ++i) {
            const auto actual = trace.accesses[i], want = wanted[i];
            if (actual.write != want.write || actual.address != want.address || actual.value != want.value) return false;
        }
        if (sp == 0 && bus.read8(0x4000) != 0x21) return false; // Low byte 45 selects bank 1.
    }
    return true;
}
bool block() {
    struct Case { uint16_t source, target; uint8_t control, first, last, flags; };
    constexpr Case cases[]{{0x3fff, 0xc100, 0, 0x23, 0x21, 0}, {0x3fff, 0x8000, 8, 0x23, 0x21, 0},
        {0xbfff, 0xc100, 8, 0x5a, 0x6b, 0x28}};
    for (const auto item : cases) {
        auto cartridge = makeCartridge();
        smsgg::MemoryBus bus(cartridge);
        TraceMemory trace(bus);
        bus.write8(0xfffd, 3); bus.write8(0xfffc, item.control);
        bus.write8(0xbfff, 0x5a); bus.write8(0xc000, 0x6b);
        auto expected = initialRegisters(); expected.af = 0; expected.hl = item.source;
        expected.de = item.target; expected.bc = 2; expected.pc = 0x1000;
        smsgg::Z80 cpu(expected); trace.accesses.clear();
        const auto first = run(cpu, {0xed, 0xb0}, &trace);
        ++expected.hl; ++expected.de; expected.bc = 1; expected.r = 0x37; expected.setF(4);
        if (!first || *first != 21 || !sameRegisters(cpu.registers(), expected)) return false;
        const auto last = run(cpu, {0xed, 0xb0}, &trace);
        ++expected.hl; ++expected.de; expected.bc = 0; expected.pc = 0x1002; expected.r = 0x39; expected.setF(item.flags);
        if (!last || *last != 16 || !sameRegisters(cpu.registers(), expected) || trace.accesses.size() != 4) return false;
        for (unsigned i = 0; i < 2; ++i) {
            const auto read = trace.accesses[i * 2], write = trace.accesses[i * 2 + 1];
            const auto value = i == 0 ? item.first : item.last;
            if (read.write || read.address != item.source + i || read.value != value || !write.write
                || write.address != item.target + i || write.value != value || bus.read8(static_cast<uint16_t>(item.target + i)) != value) return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "loads") return loads() ? 0 : 1;
        if (name == "register_writes") return registerWrites() ? 0 : 1;
        if (name == "stack") return stack() ? 0 : 1;
        if (name == "block") return block() ? 0 : 1;
    }
    std::cerr << "Failed Cartridge CPU test\n";
    return 1;
}
