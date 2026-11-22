#include "core/bus/MemoryBus.h"
#include "core/bus/IoBus.h"
#include "core/bus/MemoryControl.h"
#include "IoTestSupport.h"
#include "BusTestSupport.h"

#include <algorithm>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::MemoryBus;
using smsgg::IoBus;
using smsgg::test::Memory;
using smsgg::test::Io;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::run;
using smsgg::test::TraceMemory;

namespace {
bool memory() {
    Memory external;
    MemoryBus bus(external);
    TraceMemory trace(bus);
    for (const uint16_t address : {0xc000, 0xdfff, 0xe000, 0xffff}) {
        for (const uint8_t opcode : {0x3a, 0x32, 0x34}) {
            auto expected = initialRegisters(); expected.hl = address; expected.pc = 0xffff; expected.r = 0xff;
            Z80 cpu(expected);
            bus.write8(address, 0x7f); external.bytes[address] = 0xa5;
            trace.accesses.clear(); external.accesses.clear();
            const auto time = opcode == 0x34 ? run(cpu, {opcode}, &trace)
                : run(cpu, {opcode, static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8)}, &trace);
            expected.pc = opcode == 0x34 ? 0 : 2; expected.r = 0x80;
            if (opcode == 0x3a) expected.setA(0x7f);
            if (opcode == 0x34) expected.setF(0x95);
            if (!time || *time != (opcode == 0x34 ? 11 : 13) || !sameRegisters(cpu.registers(), expected)) return false;
            const uint8_t result = opcode == 0x3a ? 0x7f : opcode == 0x32 ? 0x12 : 0x80;
            if (bus.read8(static_cast<uint16_t>(address ^ 0x2000)) != result
                || trace.accesses.size() != (opcode == 0x34 ? 2 : 1)) return false;
            if (opcode == 0x3a || opcode == 0x34) {
                if (trace.accesses[0].write || trace.accesses[0].address != address || trace.accesses[0].value != 0x7f) return false;
            }
            if (opcode != 0x3a) {
                const auto& write = trace.accesses.back();
                if (!write.write || write.address != address || write.value != result
                    || !external.accessedOnce(true, address, result)) return false;
            } else if (!external.accesses.empty()) return false;
        }
    }
    return true;
}

bool stack() {
    Memory external;
    MemoryBus bus(external);
    TraceMemory trace(bus);
    for (const uint16_t sp : {0xc002, 0xe002, 0}) {
        auto expected = initialRegisters(); expected.sp = sp; expected.pc = 0xffff; expected.r = 0xff;
        Z80 cpu(expected); trace.accesses.clear(); external.accesses.clear();
        const auto push = run(cpu, {0xc5}, &trace), pop = run(cpu, {0xd1}, &trace);
        expected.de = expected.bc; expected.pc = 1; expected.r = 0x81;
        if (!push || *push != 11 || !pop || *pop != 10 || !sameRegisters(cpu.registers(), expected)
            || trace.accesses.size() != 4 || external.accesses.size() != 2) return false;
        const std::array<Memory::Access, 4> accesses{{
            {true, static_cast<uint16_t>(sp - 1), 0x23}, {true, static_cast<uint16_t>(sp - 2), 0x45},
            {false, static_cast<uint16_t>(sp - 2), 0x45}, {false, static_cast<uint16_t>(sp - 1), 0x23},
        }};
        for (size_t i = 0; i < accesses.size(); ++i) {
            const auto& actual = trace.accesses[i]; const auto& want = accesses[i];
            if (actual.write != want.write || actual.address != want.address || actual.value != want.value) return false;
            if (i < 2 && (!external.accesses[i].write || external.accesses[i].address != want.address
                || external.accesses[i].value != want.value)) return false;
        }
    }
    return true;
}

bool block() {
    Memory external;
    MemoryBus bus(external);
    TraceMemory trace(bus);
    for (const uint16_t source : {0xc000, 0xffff}) {
        auto expected = initialRegisters(); expected.hl = source; expected.de = source == 0xc000 ? 0xe000 : 0xc100;
        expected.bc = 2; expected.pc = 0xffff;
        const uint16_t target = expected.de;
        bus.write8(source, 0x5a); bus.write8(static_cast<uint16_t>(source + 1), 0x6b);
        external.bytes[source] = 0xa5;
        Z80 cpu(expected); trace.accesses.clear(); external.accesses.clear();
        const auto first = run(cpu, {0xed, 0xb0}, &trace);
        expected.hl = static_cast<uint16_t>(source + 1); ++expected.de; expected.bc = 1;
        expected.r = 0x37; expected.setF(0xed);
        if (!first || *first != 21 || !sameRegisters(cpu.registers(), expected)) return false;
        const auto last = run(cpu, {0xed, 0xb0}, &trace);
        expected.hl = static_cast<uint16_t>(source + 2); ++expected.de; expected.bc = 0;
        expected.pc = 1; expected.r = 0x39; expected.setF(0xc9);
        if (!last || *last != 16 || !sameRegisters(cpu.registers(), expected) || trace.accesses.size() != 4
            || bus.read8(target) != 0x5a || bus.read8(static_cast<uint16_t>(target + 1)) != 0x6b) return false;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& read = trace.accesses[i * 2]; const auto& write = trace.accesses[i * 2 + 1];
            const uint8_t value = i == 0 ? 0x5a : 0x6b;
            if (read.write || read.address != static_cast<uint16_t>(source + i) || read.value != value
                || !write.write || write.address != target + i || write.value != value) return false;
        }
        if (external.accesses.size() != (source == 0xffff ? 3 : 2)) return false;
        size_t index = 0;
        for (unsigned i = 0; i < 2; ++i) {
            if (source == 0xffff && i == 1) {
                const auto& read = external.accesses[index++];
                if (read.write || read.address != 0 || read.value != 0x6b) return false;
            }
            const auto& write = external.accesses[index++];
            if (!write.write || write.address != target + i || write.value != (i == 0 ? 0x5a : 0x6b)) return false;
        }
    }
    return true;
}

bool ports() {
    Memory external;
    MemoryBus bus(external);
    Io devices;
    IoBus io(devices);
    struct Port { uint8_t low, read; int write; };
    constexpr std::array<Port, 12> ports{{
        {0, 0x3e, 0x3e}, {1, 0x3f, 0x3f}, {0x40, 0x7e, 0x7f}, {0x41, 0x7f, 0x7f},
        {0x80, 0xbe, 0xbe}, {0x81, 0xbf, 0xbf}, {0xc0, 0xdc, -1}, {0xc1, 0xdd, -1},
        {0xf0, 0xdc, -1}, {0xf1, 0xdd, -1}, {0xf2, 0xdc, -1}, {0xff, 0xdd, -1},
    }};
    for (const uint8_t a : {0, 0x7f, 0xff}) {
        for (const auto& port : ports) {
            for (const bool output : {false, true}) {
                auto expected = initialRegisters(); expected.setA(a); expected.pc = 0xffff; expected.r = 0xff;
                Z80 cpu(expected); devices.reply = 0x5a; devices.accesses.clear();
                const std::array<uint8_t, 2> bytes{static_cast<uint8_t>(output ? 0xd3 : 0xdb), port.low};
                const auto time = cpu.executeInstruction(bytes, &bus, &io);
                expected.pc = 1; expected.r = 0x80; if (!output) expected.setA(0x5a);
                if (!time || *time != 11 || !sameRegisters(cpu.registers(), expected) || !external.accesses.empty()) return false;
                if (output && port.write == -1) {
                    if (!devices.accesses.empty()) return false;
                } else if (devices.accesses.size() != 1 || devices.accesses[0].write != output
                    || devices.accesses[0].port != (output ? port.write : port.read)
                    || devices.accesses[0].value != (output ? a : 0x5a)) return false;
            }
        }
    }
    return true;
}

bool writeTiming() {
    constexpr uint64_t start = 0x1000000e0ULL;
    Memory external;
    MemoryBus memory(external);
    Io devices;
    smsgg::MemoryControl control(memory, devices, smsgg::Machine::Sms);
    IoBus io(control);
    external.bytes[0] = 0xed;
    external.bytes[1] = 0xb3; // OTIR
    external.bytes[0x100] = 0x12;
    external.bytes[0x101] = 0x34;
    Z80 cpu({.bc = 0x0280, .hl = 0x100});
    const auto first = std::get<uint32_t>(cpu.step(memory, io, start));
    const auto last = std::get<uint32_t>(cpu.step(memory, io, start + first));
    if (first != 21 || last != 16 || devices.accesses.size() != 2) return false;
    if (!devices.accesses[0].write || devices.accesses[0].port != 0xbe
        || devices.accesses[0].value != 0x12 || devices.accesses[0].tstate != start + 16
        || !devices.accesses[1].write || devices.accesses[1].port != 0xbe
        || devices.accesses[1].value != 0x34 || devices.accesses[1].tstate != start + first + 16) return false;

    struct Output { std::array<uint8_t, 4> bytes; uint32_t tstates; };
    constexpr Output outputs[]{
        {{0xd3, 0x80}, 11}, // OUT (n),A
        {{0xed, 0x79}, 12}, // OUT (C),A
        {{0xdd, 0xfd, 0xed, 0xab}, 24}, // OUTD with two ignored prefixes
        {{0xfd, 0xd3, 0x80}, 15},
    };
    for (const auto& output : outputs) {
        std::copy(output.bytes.begin(), output.bytes.end(), external.bytes.begin());
        Z80 single({.af = 0x1200, .bc = 0x0180, .hl = 0x100});
        devices.accesses.clear();
        if (std::get<uint32_t>(single.step(memory, io, start)) != output.tstates
            || devices.accesses.size() != 1) return false;
        const auto& write = devices.accesses.front();
        if (!write.write || write.port != 0xbe || write.value != 0x12
            || write.tstate != start + output.tstates) return false;
    }
    return true;
}

bool blockIo() {
    Memory external;
    MemoryBus bus(external);
    TraceMemory trace(bus);
    Io devices;
    IoBus io(devices);
    struct Vector { uint8_t opcode, firstF, lastF; };
    constexpr std::array<Vector, 8> vectors{{
        {0xa2, 4, 0x40}, {0xaa, 0, 0x44}, {0xb2, 0, 0x40}, {0xba, 4, 0x44},
        {0xa3, 4, 0x40}, {0xab, 0x15, 0x55}, {0xb3, 0, 0x40}, {0xbb, 1, 0x55},
    }};
    for (const auto& vector : vectors) {
        auto expected = initialRegisters(); expected.bc = 0x0240; expected.hl = 0xffff;
        Z80 cpu(expected);
        const bool output = (vector.opcode & 1) != 0, repeat = (vector.opcode & 0x10) != 0;
        const int direction = (vector.opcode & 8) != 0 ? -1 : 1;
        if (output) {
            bus.write8(0xffff, 0x11); bus.write8(static_cast<uint16_t>(0xffff + direction), 0x11);
        }
        external.accesses.clear(); trace.accesses.clear(); devices.accesses.clear(); devices.reply = 0x11;
        const std::array<uint8_t, 2> bytes{0xed, vector.opcode};
        for (unsigned i = 0; i < 2; ++i) {
            const auto time = cpu.executeInstruction(bytes, &trace, &io);
            expected.setB(static_cast<uint8_t>(1 - i)); expected.hl = static_cast<uint16_t>(expected.hl + direction);
            expected.pc = static_cast<uint16_t>(0x1234 + (repeat ? (i == 0 ? 0 : 2) : 2 * (i + 1)));
            expected.r = static_cast<uint8_t>(0x37 + 2 * i); expected.setF(i == 0 ? vector.firstF : vector.lastF);
            if (!time || *time != (repeat && i == 0 ? 21 : 16) || !sameRegisters(cpu.registers(), expected)) {
                std::cerr << "block I/O opcode=" << unsigned(vector.opcode) << " iteration=" << i << '\n';
                return false;
            }
        }
        if (trace.accesses.size() != 2 || devices.accesses.size() != 2) return false;
        for (unsigned i = 0; i < 2; ++i) {
            const auto& access = trace.accesses[i]; const auto& port = devices.accesses[i];
            if (access.write == output || access.address != static_cast<uint16_t>(0xffff + direction * static_cast<int>(i))
                || access.value != 0x11 || port.write != output || port.port != (output ? 0x7f : 0x7e)
                || port.value != 0x11) return false;
        }
        const bool crossesLow = direction > 0;
        if (external.accesses.size() != (output ? (crossesLow ? 1 : 0) : 2)) return false;
        for (size_t i = 0; i < external.accesses.size(); ++i) {
            const auto& access = external.accesses[i];
            const auto address = output ? 0 : static_cast<uint16_t>(0xffff + direction * static_cast<int>(i));
            if (access.write == output || access.address != address || access.value != 0x11) return false;
        }
    }
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "memory") return memory() ? 0 : 1;
        if (name == "stack") return stack() ? 0 : 1;
        if (name == "block") return block() ? 0 : 1;
        if (name == "ports") return ports() ? 0 : 1;
        if (name == "write_timing") return writeTiming() ? 0 : 1;
        if (name == "block_io") return blockIo() ? 0 : 1;
    }
    std::cerr << "Failed bus/CPU test\n";
    return 1;
}
