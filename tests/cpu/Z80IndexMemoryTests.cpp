#include "Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::Memory;
using smsgg::test::run;

namespace {

const std::array<void (Z80Registers::*)(uint8_t), 8> set{
    &Z80Registers::setB, &Z80Registers::setC, &Z80Registers::setD, &Z80Registers::setE,
    &Z80Registers::setH, &Z80Registers::setL, nullptr, &Z80Registers::setA};
const std::vector<std::vector<uint8_t>> heads{{0xdd}, {0xfd}, {0xdd, 0xfd}, {0xfd, 0xdd}};

void advance(Z80Registers& expected, size_t length, size_t fetches) {
    expected.pc = static_cast<uint16_t>(expected.pc + length);
    const unsigned r = expected.r;
    expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + fetches) % 128);
}

bool observesAddress(Z80& cpu, Memory& memory, uint16_t address) {
    memory.bytes[cpu.registers().hl] = 0;
    const uint8_t flags = static_cast<uint8_t>((cpu.registers().f() & 1) | 0x54 | ((address / 256) & 0x28));
    return run(cpu, {0xcb, 0x46}, &memory) && cpu.registers().f() == flags;
}

bool loads() {
    Memory memory;
    for (const auto& head : heads) {
        for (const uint16_t base : {0, 0x7fff, 0xffff}) {
            for (unsigned displacement = 0; displacement < 256; ++displacement) {
                const int offset = static_cast<int>(displacement) - (displacement < 128 ? 0 : 256);
                const auto address = static_cast<uint16_t>(base + offset);
                for (const unsigned code : {0, 1, 2, 3, 4, 5, 7}) {
                    for (const uint8_t value : {0, 1, 0x7f, 0x80, 0xff, 0x28}) {
                        for (unsigned mode = 0; mode < 3; ++mode) {
                            if (mode == 2 && code != 0) continue;
                            auto expected = initialRegisters();
                            expected.ix = head.back() == 0xdd ? base : 0x1234;
                            expected.iy = head.back() == 0xfd ? base : 0x1234;
                            expected.af = static_cast<uint16_t>(0x1200 + displacement);
                            expected.pc = 0xffff;
                            expected.r = static_cast<uint8_t>(displacement);
                            if (mode == 1) (expected.*set[code])(value);
                            Z80 cpu(expected);
                            auto bytes = head;
                            bytes.push_back(static_cast<uint8_t>(mode == 0 ? 0x46 + code * 8 : mode == 1 ? 0x70 + code : 0x36));
                            bytes.push_back(static_cast<uint8_t>(displacement));
                            if (mode == 2) bytes.push_back(value);
                            memory.bytes[address] = mode == 0 ? value : static_cast<uint8_t>(255 - value);
                            memory.accesses.clear();
                            const auto elapsed = cpu.executeInstruction(bytes, &memory);
                            if (mode == 0) (expected.*set[code])(value);
                            advance(expected, bytes.size(), head.size() + 1);
                            if (!elapsed || *elapsed != 15 + head.size() * 4
                                || !sameRegisters(cpu.registers(), expected)
                                || !memory.accessedOnce(mode != 0, address, value) || memory.bytes[address] != value) return false;
                            const unsigned xy = expected.a() | expected.f();
                            if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)
                                || !observesAddress(cpu, memory, address)) return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}

bool arithmetic() {
    Memory memory;
    for (const auto& head : heads) {
        for (const uint16_t base : {0, 0x7fff, 0xffff}) {
            for (unsigned displacement = 0; displacement < 256; ++displacement) {
                const int offset = static_cast<int>(displacement) - (displacement < 128 ? 0 : 256);
                const auto address = static_cast<uint16_t>(base + offset);
                for (unsigned operation = 0; operation < 10; ++operation) {
                    for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                        auto expected = initialRegisters();
                        expected.ix = head.back() == 0xdd ? base : 0x1234;
                        expected.iy = head.back() == 0xfd ? base : 0x1234;
                        expected.af = static_cast<uint16_t>(0x8000 + flags);
                        expected.pc = 0xffff;
                        expected.r = static_cast<uint8_t>(displacement);
                        Z80 cpu(expected);
                        auto normal = expected;
                        normal.setB(static_cast<uint8_t>(displacement));
                        Z80 reference(normal);
                        const uint8_t regOp = static_cast<uint8_t>(operation < 8 ? 0x80 + operation * 8 : operation == 8 ? 4 : 5);
                        if (!reference.executeOpcode(regOp)) return false;
                        expected.af = reference.registers().af;
                        const uint8_t result = reference.registers().b();
                        auto bytes = head;
                        bytes.push_back(static_cast<uint8_t>(operation < 8 ? 0x86 + operation * 8 : operation == 8 ? 0x34 : 0x35));
                        bytes.push_back(static_cast<uint8_t>(displacement));
                        memory.bytes[address] = static_cast<uint8_t>(displacement);
                        memory.accesses.clear();
                        const auto elapsed = cpu.executeInstruction(bytes, &memory);
                        advance(expected, bytes.size(), head.size() + 1);
                        if (!elapsed || *elapsed != (operation < 8 ? 15 : 19) + head.size() * 4
                            || !sameRegisters(cpu.registers(), expected) || memory.bytes[address] != result
                            || memory.accesses.size() != (operation < 8 ? 1 : 2)
                            || memory.accesses[0].write || memory.accesses[0].address != address
                            || memory.accesses[0].value != displacement) return false;
                        if (operation >= 8 && (!memory.accesses[1].write || memory.accesses[1].address != address
                            || memory.accesses[1].value != result)) return false;
                        if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (expected.a() & 0x28)
                            || !observesAddress(cpu, memory, address)) return false;
                    }
                }
            }
        }
    }
    return true;
}

bool cb(unsigned group) {
    Memory memory;
    for (const auto& head : heads) {
        for (const uint16_t base : {0, 0x0800, 0x2000, 0x7fff, 0xffff}) {
            for (unsigned opcode = group * 64; opcode < (group + 1) * 64; ++opcode) {
                for (unsigned value = 0; value < 256; ++value) {
                    const int offset = static_cast<int>(value) - (value < 128 ? 0 : 256);
                    const auto address = static_cast<uint16_t>(base + offset);
                    for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                        auto expected = initialRegisters();
                        expected.ix = head.back() == 0xdd ? base : 0x1234;
                        expected.iy = head.back() == 0xfd ? base : 0x1234;
                        expected.af = static_cast<uint16_t>(0x1200 + flags);
                        expected.pc = 0xffff;
                        expected.r = static_cast<uint8_t>(value);
                        Z80 cpu(expected);
                        auto normal = expected;
                        normal.setB(static_cast<uint8_t>(value));
                        Z80 reference(normal);
                        if (!reference.executeOpcode(0xcb, static_cast<uint8_t>(opcode / 8 * 8))) return false;
                        const uint8_t result = reference.registers().b();
                        expected.af = reference.registers().af;
                        if (group == 1) expected.setF(static_cast<uint8_t>((expected.f() & 0xd7) | ((address / 256) & 0x28)));
                        else if (opcode % 8 != 6) (expected.*set[opcode % 8])(result);
                        auto bytes = head;
                        bytes.insert(bytes.end(), {0xcb, static_cast<uint8_t>(value), static_cast<uint8_t>(opcode)});
                        memory.bytes[address] = static_cast<uint8_t>(value);
                        memory.accesses.clear();
                        const auto elapsed = cpu.executeInstruction(bytes, &memory);
                        advance(expected, bytes.size(), head.size() + 1);
                        if (!elapsed || *elapsed != (group == 1 ? 16 : 19) + head.size() * 4
                            || !sameRegisters(cpu.registers(), expected) || memory.bytes[address] != result
                            || memory.accesses.size() != (group == 1 ? 1 : 2)
                            || memory.accesses[0].write || memory.accesses[0].address != address
                            || memory.accesses[0].value != value) return false;
                        if (group != 1 && (!memory.accesses[1].write || memory.accesses[1].address != address
                            || memory.accesses[1].value != result)) return false;
                        const unsigned xy = expected.a() | (group < 2 ? 0 : expected.f());
                        if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)
                            || !observesAddress(cpu, memory, address)) return false;
                    }
                }
            }
        }
    }
    return true;
}

bool missing() {
    Memory memory;
    const std::vector<std::vector<uint8_t>> cases{{0xdd, 0x46}, {0xfd, 0x70}, {0xdd, 0xfd},
        {0xdd, 0x36}, {0xdd, 0x36, 0x80}, {0xfd, 0x86}, {0xfd, 0x34}, {0xfd, 0x35},
        {0xdd, 0xcb}, {0xfd, 0xcb, 0x80}};
    for (const auto& bytes : cases) {
        auto initial = initialRegisters();
        initial.af = 0;
        initial.bc = 0x2828;
        Z80 cpu(initial);
        if (!run(cpu, {0xc3, 0, 0x28}, &memory) || !run(cpu, {0xfb}, &memory)
            || !run(cpu, {0xcb, 0x40}, &memory)) return false;
        const auto before = cpu.registers();
        memory.accesses.clear();
        if (cpu.executeInstruction(bytes, &memory) || !sameRegisters(cpu.registers(), before)
            || !cpu.iff1() || !cpu.iff2() || !cpu.canAcceptMaskableInterrupt() || !memory.accesses.empty()) return false;
        if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != 0
            || !observesAddress(cpu, memory, 0x2800)) return false;
    }
    for (const uint8_t opcode : {0x46, 0x70, 0x36, 0x86, 0x34, 0x35, 0xcb}) {
        Z80 cpu(initialRegisters());
        const auto before = cpu.registers();
        if (run(cpu, {0xdd, opcode, 0, 0x12}, nullptr) || !sameRegisters(cpu.registers(), before)) return false;
    }
    Z80 enabled(initialRegisters());
    return run(enabled, {0xfb}, &memory) && !run(enabled, {0xdd, 0x46}, &memory)
        && !enabled.canAcceptMaskableInterrupt() && run(enabled, {0xdd, 0xfd, 0x46, 0}, &memory)
        && enabled.canAcceptMaskableInterrupt();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "cb_shift") return cb(0) ? 0 : 1;
        if (name == "cb_bit") return cb(1) ? 0 : 1;
        if (name == "cb_res") return cb(2) ? 0 : 1;
        if (name == "cb_set") return cb(3) ? 0 : 1;
        const bool passed = name == "ld" ? loads() : name == "alu" ? arithmetic() : name == "missing" && missing();
        if (passed) return 0;
    }
    std::cerr << "Failed Z80 indexed memory test\n";
    return 1;
}
