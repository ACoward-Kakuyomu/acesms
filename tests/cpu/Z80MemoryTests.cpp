#include "Z80MemoryTestSupport.h"

#include <array>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::Memory;
using smsgg::test::run;

namespace {

struct Register {
    uint8_t code;
    uint8_t (Z80Registers::*get)() const;
    void (Z80Registers::*set)(uint8_t);
};
const std::array<Register, 7> registers{{
    {0, &Z80Registers::b, &Z80Registers::setB}, {1, &Z80Registers::c, &Z80Registers::setC},
    {2, &Z80Registers::d, &Z80Registers::setD}, {3, &Z80Registers::e, &Z80Registers::setE},
    {4, &Z80Registers::h, &Z80Registers::setH}, {5, &Z80Registers::l, &Z80Registers::setL},
    {7, &Z80Registers::a, &Z80Registers::setA},
}};

bool transfers(bool write) {
    Memory memory;
    for (const auto& reg : registers) {
        for (unsigned value = 0; value < 256; ++value) {
            for (const uint16_t address : {0, 0x7fff, 0xffff}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + value);
                expected.hl = address;
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(value);
                if (write) (expected.*reg.set)(static_cast<uint8_t>(value));
                const uint16_t effective = expected.hl;
                Z80 cpu(expected);
                memory.bytes[effective] = write ? 0x55 : static_cast<uint8_t>(value);
                memory.accesses.clear();
                if (!write) (expected.*reg.set)(static_cast<uint8_t>(value));
                expected.pc = 0;
                expected.r = static_cast<uint8_t>(value / 128 * 128 + (value % 128 + 1) % 128);
                const auto elapsed = run(cpu, {static_cast<uint8_t>(write ? 0x70 + reg.code : 0x46 + reg.code * 8)}, &memory);
                if (!elapsed || *elapsed != 7 || !sameRegisters(cpu.registers(), expected)
                    || !memory.accessedOnce(write, effective, static_cast<uint8_t>(value))
                    || memory.bytes[effective] != value) return false;
                // LD preserves F and clears Q regardless of whether a memory operand was used.
                const unsigned xy = expected.a() | expected.f();
                if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)) return false;
            }
        }
    }
    return true;
}

bool immediate() {
    Memory memory;
    for (unsigned value = 0; value < 256; ++value) {
        for (const uint16_t address : {0, 0x7fff, 0xffff}) {
            auto expected = initialRegisters();
            expected.hl = address;
            expected.pc = 0xffff;
            expected.r = static_cast<uint8_t>(value);
            Z80 cpu(expected);
            memory.accesses.clear();
            const auto elapsed = cpu.executeOpcode(0x36, static_cast<uint8_t>(value), std::nullopt, &memory);
            expected.pc = 1;
            expected.r = static_cast<uint8_t>(value / 128 * 128 + (value % 128 + 1) % 128);
            if (!elapsed || *elapsed != 10 || !sameRegisters(cpu.registers(), expected)
                || !memory.accessedOnce(true, address, static_cast<uint8_t>(value))
                || memory.bytes[address] != value) return false;
        }
    }
    auto initial = initialRegisters();
    initial.af = 0;
    initial.bc = 0x2828;
    Z80 cpu(initial);
    return run(cpu, {0xcb, 0x40}, &memory) && run(cpu, {0x36, 0xa5}, &memory)
        && run(cpu, {0x37}, &memory) && (cpu.registers().f() & 0x28) == 0x28;
}

bool incrementDecrement(bool decrement) {
    Memory memory;
    for (unsigned value = 0; value < 256; ++value) {
        for (unsigned flags = 0; flags < 256; ++flags) {
            for (const uint16_t address : {0, 0x7fff, 0xffff}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + flags);
                expected.hl = address;
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(value);
                Z80 cpu(expected);
                auto normal = expected;
                normal.setB(static_cast<uint8_t>(value));
                Z80 reference(normal);
                if (!reference.executeOpcode(decrement ? 0x05 : 0x04)) return false;
                const uint8_t result = reference.registers().b();
                expected.af = reference.registers().af;
                expected.pc = 0;
                expected.r = static_cast<uint8_t>(value / 128 * 128 + (value % 128 + 1) % 128);
                memory.bytes[address] = static_cast<uint8_t>(value);
                memory.accesses.clear();
                const auto elapsed = run(cpu, {static_cast<uint8_t>(decrement ? 0x35 : 0x34)}, &memory);
                if (!elapsed || *elapsed != 11 || !sameRegisters(cpu.registers(), expected)
                    || memory.bytes[address] != result || memory.accesses.size() != 2
                    || memory.accesses[0].write || memory.accesses[0].address != address
                    || memory.accesses[0].value != value || !memory.accesses[1].write
                    || memory.accesses[1].address != address || memory.accesses[1].value != result) return false;
                if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (expected.a() & 0x28)) return false;
            }
        }
    }
    Z80 cpu(initialRegisters());
    const auto initial = cpu.registers();
    memory.accesses.clear();
    return !run(cpu, {static_cast<uint8_t>(decrement ? 0x35 : 0x34)}, nullptr)
        && !run(cpu, {0xdd, static_cast<uint8_t>(decrement ? 0x35 : 0x34)}, &memory)
        && sameRegisters(cpu.registers(), initial) && memory.accesses.empty();
}

bool alu(unsigned operation) {
    Memory memory;
    for (unsigned operand = 0; operand < 256; ++operand) {
        for (const unsigned a : {0, 1, 0x7f, 0x80, 0xff, 0x55, 0xaa}) {
            for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                for (const uint16_t address : {0, 0x7fff, 0xffff}) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(a * 256 + flags);
                    expected.hl = address;
                    expected.pc = 0xffff;
                    expected.r = static_cast<uint8_t>(operand);
                    Z80 cpu(expected);
                    auto normal = expected;
                    normal.setB(static_cast<uint8_t>(operand));
                    Z80 reference(normal);
                    if (!reference.executeOpcode(static_cast<uint8_t>(0x80 + operation * 8))) return false;
                    expected.af = reference.registers().af;
                    expected.pc = 0;
                    expected.r = static_cast<uint8_t>(operand / 128 * 128 + (operand % 128 + 1) % 128);
                    memory.bytes[address] = static_cast<uint8_t>(operand);
                    memory.accesses.clear();
                    const auto elapsed = run(cpu, {static_cast<uint8_t>(0x86 + operation * 8)}, &memory);
                    if (!elapsed || *elapsed != 7 || !sameRegisters(cpu.registers(), expected)
                        || !memory.accessedOnce(false, address, static_cast<uint8_t>(operand))
                        || memory.bytes[address] != operand) return false;
                    if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (expected.a() & 0x28)) return false;
                }
            }
        }
    }
    Z80 cpu(initialRegisters());
    const auto initial = cpu.registers();
    const auto opcode = static_cast<uint8_t>(0x86 + operation * 8);
    memory.accesses.clear();
    return !run(cpu, {opcode}, nullptr) && !run(cpu, {0xfd, opcode}, &memory)
        && sameRegisters(cpu.registers(), initial) && memory.accesses.empty();
}

bool directTransfers(bool write) {
    Memory memory;
    const std::array<uint8_t, 3> loads{0x0a, 0x1a, 0x3a};
    const std::array<uint8_t, 3> stores{0x02, 0x12, 0x32};
    for (unsigned mode = 0; mode < 3; ++mode) {
        const auto opcode = write ? stores[mode] : loads[mode];
        for (unsigned address = 0; address < 65536; ++address) {
            for (const uint8_t prefix : {0, 0xdd, 0xfd}) {
                const auto value = static_cast<uint8_t>(address);
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>((write ? value : 0x12) * 256 + value);
                if (mode == 0) expected.bc = static_cast<uint16_t>(address);
                if (mode == 1) expected.de = static_cast<uint16_t>(address);
                expected.pc = 0xffff;
                expected.r = value;
                Z80 cpu(expected);
                memory.bytes[address] = write ? 0x55 : value;
                memory.accesses.clear();
                std::vector<uint8_t> bytes;
                if (prefix != 0) bytes.push_back(prefix);
                bytes.push_back(opcode);
                if (mode == 2) { bytes.push_back(value); bytes.push_back(static_cast<uint8_t>(address / 256)); }
                const auto elapsed = cpu.executeInstruction(bytes, &memory);
                if (!write) expected.setA(value);
                expected.pc = static_cast<uint16_t>(0xffff + bytes.size());
                const unsigned fetches = prefix != 0 ? 2 : 1;
                expected.r = static_cast<uint8_t>(value / 128 * 128 + (value % 128 + fetches) % 128);
                if (!elapsed || *elapsed != (mode == 2 ? 13 : 7) + (prefix != 0 ? 4 : 0)
                    || !sameRegisters(cpu.registers(), expected)
                    || !memory.accessedOnce(write, static_cast<uint16_t>(address), value)
                    || memory.bytes[address] != value) return false;
            }
        }
        Z80 cpu(initialRegisters());
        const auto before = cpu.registers();
        memory.accesses.clear();
        if (run(cpu, {opcode, 0xff, 0xff}, nullptr)) return false;
        if (mode == 2 && (run(cpu, {opcode}, &memory) || run(cpu, {opcode, 0xff}, &memory))) return false;
        if (!sameRegisters(cpu.registers(), before) || !memory.accesses.empty()) return false;
    }
    return true;
}

bool wordTransfers(bool write) {
    struct Form { std::vector<uint8_t> head; uint16_t Z80Registers::*pair; unsigned cycles; };
    const uint8_t base = write ? 0x22 : 0x2a;
    const uint8_t ed = write ? 0x43 : 0x4b;
    const std::array<Form, 8> forms{{
        {{base}, &Z80Registers::hl, 16}, {{0xdd, base}, &Z80Registers::ix, 20},
        {{0xfd, base}, &Z80Registers::iy, 20}, {{0xed, ed}, &Z80Registers::bc, 20},
        {{0xed, static_cast<uint8_t>(ed + 0x10)}, &Z80Registers::de, 20},
        {{0xed, static_cast<uint8_t>(ed + 0x20)}, &Z80Registers::hl, 20},
        {{0xed, static_cast<uint8_t>(ed + 0x30)}, &Z80Registers::sp, 20},
        {{0xdd, 0xfd, 0xed, static_cast<uint8_t>(ed + 0x20)}, &Z80Registers::hl, 28},
    }};
    Memory memory;
    for (const auto& form : forms) {
        for (unsigned word = 0; word < 65536; ++word) {
            for (const uint16_t address : {0, 0x7fff, 0xffff}) {
                const auto next = static_cast<uint16_t>(address + 1);
                const auto low = static_cast<uint8_t>(word);
                const auto high = static_cast<uint8_t>(word / 256);
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + low);
                expected.*form.pair = static_cast<uint16_t>(write ? word : 65535 - word);
                expected.pc = 0xffff;
                expected.r = low;
                Z80 cpu(expected);
                memory.bytes[address] = write ? static_cast<uint8_t>(255 - low) : low;
                memory.bytes[next] = write ? static_cast<uint8_t>(255 - high) : high;
                memory.accesses.clear();
                auto bytes = form.head;
                bytes.push_back(static_cast<uint8_t>(address));
                bytes.push_back(static_cast<uint8_t>(address / 256));
                const auto elapsed = cpu.executeInstruction(bytes, &memory);
                if (!write) expected.*form.pair = static_cast<uint16_t>(word);
                expected.pc = static_cast<uint16_t>(0xffff + bytes.size());
                expected.r = static_cast<uint8_t>(low / 128 * 128 + (low % 128 + form.head.size()) % 128);
                if (!elapsed || *elapsed != form.cycles || !sameRegisters(cpu.registers(), expected)
                    || memory.bytes[address] != low || memory.bytes[next] != high || memory.accesses.size() != 2
                    || memory.accesses[0].write != write || memory.accesses[0].address != address
                    || memory.accesses[0].value != low || memory.accesses[1].write != write
                    || memory.accesses[1].address != next || memory.accesses[1].value != high) return false;
            }
        }
        auto bytes = form.head;
        bytes.insert(bytes.end(), {0xff, 0xff});
        Z80 cpu(initialRegisters());
        const auto initial = cpu.registers();
        memory.accesses.clear();
        if (cpu.executeInstruction(bytes) || !sameRegisters(cpu.registers(), initial)) return false;
        for (size_t length = 1; length < bytes.size(); ++length) {
            if (cpu.executeInstruction(std::span<const uint8_t>(bytes.data(), length), &memory)
                || !sameRegisters(cpu.registers(), initial) || !memory.accesses.empty()) return false;
        }
    }
    return true;
}

bool digitRotate(bool left) {
    Memory memory;
    const uint8_t opcode = left ? 0x6f : 0x67;
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned operand = 0; operand < 256; ++operand) {
            for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                for (const uint16_t address : {0, 0x7fff, 0xffff}) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(a * 256 + flags);
                    expected.hl = address;
                    expected.pc = 0xffff;
                    expected.r = static_cast<uint8_t>(operand);
                    Z80 cpu(expected);
                    const unsigned resultA = a / 16 * 16 + (left ? operand / 16 : operand % 16);
                    const unsigned resultMemory = left ? operand % 16 * 16 + a % 16 : a % 16 * 16 + operand / 16;
                    unsigned ones = 0;
                    for (unsigned bits = resultA; bits != 0; bits /= 2) ones += bits % 2;
                    const unsigned resultFlags = (resultA >= 128 ? 0x80 : 0) + (resultA == 0 ? 0x40 : 0)
                        + (resultA / 32 % 2) * 0x20 + (resultA / 8 % 2) * 0x08
                        + (ones % 2 == 0 ? 4 : 0) + flags % 2;
                    expected.af = static_cast<uint16_t>(resultA * 256 + resultFlags);
                    expected.pc = 1;
                    expected.r = static_cast<uint8_t>(operand / 128 * 128 + (operand % 128 + 2) % 128);
                    memory.bytes[address] = static_cast<uint8_t>(operand);
                    memory.accesses.clear();
                    const auto elapsed = run(cpu, {0xed, opcode}, &memory);
                    if (!elapsed || *elapsed != 18 || !sameRegisters(cpu.registers(), expected)
                        || memory.bytes[address] != resultMemory || memory.accesses.size() != 2
                        || memory.accesses[0].write || memory.accesses[0].address != address
                        || memory.accesses[0].value != operand || !memory.accesses[1].write
                        || memory.accesses[1].address != address || memory.accesses[1].value != resultMemory) return false;
                }
            }
        }
    }
    for (const uint8_t prefix : {0xdd, 0xfd}) {
        auto expected = initialRegisters();
        Z80 cpu(expected);
        memory.bytes[expected.hl] = 0xab;
        memory.accesses.clear();
        const auto elapsed = run(cpu, {prefix, 0xed, opcode}, &memory);
        expected.af = left ? 0x1a09 : 0x1b0d;
        expected.pc += 3;
        expected.r += 3;
        if (!elapsed || *elapsed != 22 || !sameRegisters(cpu.registers(), expected)
            || memory.accesses.size() != 2 || memory.accesses[0].address != expected.hl) return false;
    }
    Z80 cpu(initialRegisters());
    const auto initial = cpu.registers();
    return !run(cpu, {0xed, opcode}, nullptr) && sameRegisters(cpu.registers(), initial);
}

bool cbMemory(unsigned group) {
    Memory memory;
    for (unsigned operation = 0; operation < 8; ++operation) {
        for (unsigned value = 0; value < 256; ++value) {
            for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                for (const unsigned high : {0, 0x08, 0x20, 0x28, 0xff}) {
                    for (const uint16_t address : {0, 0x7fff, 0xffff}) {
                        auto initial = initialRegisters();
                        initial.af = static_cast<uint16_t>(0x1200 + flags);
                        initial.hl = address;
                        initial.r = static_cast<uint8_t>(value);
                        Z80 cpu(initial);
                        if (!run(cpu, {0xc3, 0xff, static_cast<uint8_t>(high)}, &memory)) return false;
                        auto expected = cpu.registers();
                        auto normal = expected;
                        normal.setB(static_cast<uint8_t>(value));
                        Z80 reference(normal);
                        const uint8_t opcode = static_cast<uint8_t>(group * 64 + operation * 8 + 6);
                        if (!reference.executeOpcode(0xcb, static_cast<uint8_t>(opcode - 6))) return false;
                        const uint8_t result = reference.registers().b();
                        expected.af = reference.registers().af;
                        if (group == 1) expected.setF(static_cast<uint8_t>((expected.f() & 0xd7) | (high & 0x28)));
                        expected.pc = static_cast<uint16_t>(expected.pc + 2);
                        const unsigned r = expected.r;
                        expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + 2) % 128);
                        memory.bytes[address] = static_cast<uint8_t>(value);
                        memory.accesses.clear();
                        const auto elapsed = run(cpu, {0xcb, opcode}, &memory);
                        if (!elapsed || *elapsed != (group == 1 ? 12 : 15)
                            || !sameRegisters(cpu.registers(), expected)
                            || memory.bytes[address] != result || memory.accesses.size() != (group == 1 ? 1 : 2)
                            || memory.accesses[0].write || memory.accesses[0].address != address
                            || memory.accesses[0].value != value) return false;
                        if (group != 1 && (!memory.accesses[1].write || memory.accesses[1].address != address
                            || memory.accesses[1].value != result)) return false;
                        const unsigned xy = expected.a() | (group < 2 ? 0 : expected.f());
                        if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)) return false;
                    }
                }
            }
        }
    }
    return true;
}

bool memptrSequences() {
    struct Case { std::vector<uint8_t> bytes; uint16_t expected; };
    Memory memory;
    for (const uint16_t base : {0, 0xff, 0x0100, 0x07ff, 0x0800, 0x1fff, 0x2000, 0x27ff, 0x2800, 0x7fff, 0xffff}) {
        for (const unsigned a : {0, 8, 0x20, 0x28, 0xff}) {
            for (const unsigned flags : {0, 0xff}) {
                auto initial = initialRegisters();
                initial.af = static_cast<uint16_t>(a * 256 + flags);
                initial.hl = initial.bc = initial.de = base;
                initial.ix = static_cast<uint16_t>(base + 0x0800);
                initial.iy = static_cast<uint16_t>(base + 0x2000);
                const auto low = static_cast<uint8_t>(base);
                const auto high = static_cast<uint8_t>(base / 256);
                const auto next = static_cast<uint16_t>(base + 1);
                const auto stored = static_cast<uint16_t>(a * 256 + (base + 1) % 256);
                constexpr uint16_t seed = 0x2800;
                const std::vector<Case> cases{
                    {{0x00}, seed}, {{0x21, low, high}, seed}, {{0x46}, seed}, {{0x77}, seed},
                    {{0x34}, seed}, {{0x86}, seed}, {{0xeb}, seed}, {{0xd9}, seed}, {{0xe9}, seed},
                    {{0xdd, 0xe9}, seed}, {{0xcb, 0x00}, seed}, {{0xed, 0x57}, seed}, {{0xed, 0x5e}, seed},
                    {{0xc3, low, high}, base}, {{0xc2, low, high}, base}, {{0xca, low, high}, base},
                    {{0x18, 0x80}, seed + 2 - 128},
                    {{0x28, 0x80}, static_cast<uint16_t>(flags != 0 ? seed + 2 - 128 : seed)},
                    {{0x10, 0x80}, static_cast<uint16_t>(initial.b() != 1 ? seed + 2 - 128 : seed)},
                    {{0x09}, next}, {{0xdd, 0x09}, static_cast<uint16_t>(initial.ix + 1)},
                    {{0xfd, 0x39}, static_cast<uint16_t>(initial.iy + 1)},
                    {{0xed, 0x4a}, next}, {{0xed, 0x42}, next}, {{0xdd, 0xed, 0x4a}, next},
                    {{0x0a}, next}, {{0x1a}, next}, {{0x3a, low, high}, next},
                    {{0x02}, stored}, {{0x12}, stored}, {{0x32, low, high}, stored},
                    {{0x22, low, high}, next}, {{0x2a, low, high}, next},
                    {{0xdd, 0x22, low, high}, next}, {{0xfd, 0x2a, low, high}, next},
                    {{0xed, 0x43, low, high}, next}, {{0xed, 0x4b, low, high}, next},
                    {{0xed, 0x67}, next}, {{0xed, 0x6f}, next},
                };
                for (const auto& test : cases) {
                    Z80 cpu(initial);
                    memory.bytes.fill(0x11);
                    if (!run(cpu, {0xc3, 0, 0x28}, &memory) || !cpu.executeInstruction(test.bytes, &memory)) return false;
                    const uint16_t address = cpu.registers().hl;
                    memory.bytes[address] = 0;
                    const uint8_t expectedF = static_cast<uint8_t>((cpu.registers().f() & 1) | 0x54 | ((test.expected / 256) & 0x28));
                    if (!run(cpu, {0xcb, 0x7e}, &memory) || cpu.registers().f() != expectedF) {
                        std::cerr << "MEMPTR opcode=" << unsigned(test.bytes[0]) << " base=" << base << '\n';
                        return false;
                    }
                    // CPI exposes the low bits through the next carry into BIT's F3 source.
                    const unsigned steps = 0x800 - test.expected % 0x800;
                    for (unsigned step = 1; step <= steps; ++step) {
                        memory.accesses.clear();
                        if (!run(cpu, {0xed, 0xa1}, &memory)) return false;
                        memory.bytes[cpu.registers().hl] = 0;
                        const uint8_t xy = static_cast<uint8_t>(((test.expected + step) / 256) & 0x28);
                        if (!run(cpu, {0xcb, 0x46}, &memory) || (cpu.registers().f() & 0x28) != xy) {
                            std::cerr << "MEMPTR carry opcode=" << unsigned(test.bytes[0])
                                << " base=" << base << " step=" << step << '\n';
                            return false;
                        }
                    }
                }
            }
        }
    }
    auto initial = initialRegisters();
    initial.hl = 0x2800;
    Z80 cpu(initial);
    if (!run(cpu, {0xc3, 0, 0x28}, &memory)) return false;
    const auto before = cpu.registers();
    if (run(cpu, {0xed, 0x43, 0xff}, &memory) || !sameRegisters(cpu.registers(), before)) return false;
    memory.bytes[initial.hl] = 0;
    if (!run(cpu, {0xcb, 0x46}, &memory) || (cpu.registers().f() & 0x28) != 0x28) return false;
    cpu.reset();
    return run(cpu, {0xcb, 0x46}, &memory) && (cpu.registers().f() & 0x28) == 0;
}

bool missing() {
    Memory memory;
    const auto initial = initialRegisters();
    for (const auto& reg : registers) {
        for (const uint8_t opcode : {static_cast<uint8_t>(0x46 + reg.code * 8), static_cast<uint8_t>(0x70 + reg.code)}) {
            Z80 cpu(initial);
            if (run(cpu, {opcode}, nullptr) || !sameRegisters(cpu.registers(), initial)) return false;
            for (const uint8_t prefix : {0xdd, 0xfd}) {
                if (run(cpu, {prefix, opcode}, &memory)
                    || !sameRegisters(cpu.registers(), initial)) return false;
            }
        }
    }
    Z80 cpu(initial);
    if (run(cpu, {0x36}, &memory) || run(cpu, {0x36, 0x12}, nullptr)
        || run(cpu, {0xdd, 0x36, 0}, &memory)
        || !sameRegisters(cpu.registers(), initial) || !memory.accesses.empty()) return false;
    if (!run(cpu, {0xfb}, &memory) || run(cpu, {0x36}, &memory)
        || cpu.canAcceptMaskableInterrupt()) return false;
    if (!run(cpu, {0x46}, &memory) || !cpu.canAcceptMaskableInterrupt()) return false;
    // HALT remains distinct from LD (HL),(HL) and never accesses memory.
    memory.accesses.clear();
    const auto elapsed = run(cpu, {0x76}, &memory);
    return elapsed && *elapsed == 4 && cpu.halted() && memory.accesses.empty();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "direct_read") return directTransfers(false) ? 0 : 1;
        if (name == "direct_write") return directTransfers(true) ? 0 : 1;
        if (name == "word_read") return wordTransfers(false) ? 0 : 1;
        if (name == "word_write") return wordTransfers(true) ? 0 : 1;
        if (name == "rld") return digitRotate(true) ? 0 : 1;
        if (name == "rrd") return digitRotate(false) ? 0 : 1;
        if (name == "cb_shift") return cbMemory(0) ? 0 : 1;
        if (name == "cb_bit") return cbMemory(1) ? 0 : 1;
        if (name == "cb_res") return cbMemory(2) ? 0 : 1;
        if (name == "cb_set") return cbMemory(3) ? 0 : 1;
        if (name == "memptr") return memptrSequences() ? 0 : 1;
        const std::array<std::string_view, 8> operations{
            "alu_add", "alu_adc", "alu_sub", "alu_sbc", "alu_and", "alu_xor", "alu_or", "alu_cp"};
        for (unsigned operation = 0; operation < operations.size(); ++operation) {
            if (name == operations[operation]) return alu(operation) ? 0 : 1;
        }
        const bool passed = name == "read" ? transfers(false) : name == "write" ? transfers(true)
            : name == "immediate" ? immediate() : name == "missing" ? missing()
            : name == "inc" ? incrementDecrement(false) : name == "dec" && incrementDecrement(true);
        if (passed) return 0;
    }
    std::cerr << "Failed Z80 memory test\n";
    return 1;
}
