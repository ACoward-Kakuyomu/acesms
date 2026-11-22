#include "Z80TestSupport.h"

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

namespace {

std::optional<uint32_t> run(Z80& cpu, std::initializer_list<uint8_t> bytes) {
    return cpu.executeInstruction(std::span<const uint8_t>(bytes.begin(), bytes.size()));
}

uint16_t& index(Z80Registers& r, uint8_t prefix) { return prefix == 0xdd ? r.ix : r.iy; }

uint8_t byte(Z80Registers r, unsigned code, uint8_t prefix) {
    const std::array<uint8_t, 8> values{r.b(), r.c(), r.d(), r.e(),
        static_cast<uint8_t>(index(r, prefix) / 256),
        static_cast<uint8_t>(index(r, prefix) % 256), 0, r.a()};
    return values[code];
}

void setByte(Z80Registers& r, unsigned code, uint8_t prefix, unsigned value) {
    const std::array<uint16_t*, 8> pairs{&r.bc, &r.bc, &r.de, &r.de,
        &index(r, prefix), &index(r, prefix), nullptr, &r.af};
    auto& pair = *pairs[code]; // Test cases use the seven register codes, never memory code 6.
    pair = static_cast<uint16_t>(code == 7 || code % 2 == 0
        ? value * 256 + pair % 256 : pair / 256 * 256 + value);
}

void advance(Z80Registers& expected, unsigned length, unsigned fetches) {
    expected.pc = static_cast<uint16_t>(expected.pc + length);
    const unsigned r = expected.r;
    expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + fetches) % 128);
}

bool loads() {
    for (const uint8_t prefix : {0xdd, 0xfd}) {
        for (const unsigned target : {0, 1, 2, 3, 4, 5, 7}) {
            for (const unsigned source : {0, 1, 2, 3, 4, 5, 7}) {
                for (unsigned value = 0; value < 256; ++value) {
                    auto expected = initialRegisters();
                    expected.pc = 0xffff;
                    expected.r = static_cast<uint8_t>(value);
                    setByte(expected, source, prefix, value);
                    Z80 cpu(expected);
                    setByte(expected, target, prefix, byte(expected, source, prefix));
                    advance(expected, 2, 2);
                    const auto elapsed = run(cpu, {prefix, static_cast<uint8_t>(0x40 + target * 8 + source)});
                    if (!elapsed || *elapsed != 8 || !sameRegisters(cpu.registers(), expected)) return false;
                }
            }
            for (unsigned value = 0; value < 256; ++value) {
                auto expected = initialRegisters();
                expected.pc = 0xfffe;
                Z80 cpu(expected);
                setByte(expected, target, prefix, value);
                advance(expected, 3, 2);
                const auto elapsed = cpu.executeOpcode(prefix,
                    static_cast<uint8_t>(0x06 + target * 8), static_cast<uint8_t>(value));
                if (!elapsed || *elapsed != 11 || !sameRegisters(cpu.registers(), expected)) return false;
            }
        }
    }
    return true;
}

bool alu() {
    for (const uint8_t prefix : {0xdd, 0xfd}) {
        for (unsigned value = 0; value < 256; ++value) {
            for (const uint8_t flags : {0, 1, 0xfe, 0xff}) {
                for (const unsigned code : {4, 5}) {
                    for (const unsigned operation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
                        auto expected = initialRegisters();
                        expected.af = static_cast<uint16_t>(0x8000 + flags);
                        expected.pc = 0xffff;
                        expected.r = static_cast<uint8_t>(value);
                        setByte(expected, code, prefix, value);
                        Z80 cpu(expected);
                        auto normal = expected;
                        normal.hl = index(expected, prefix);
                        Z80 reference(normal);
                        const uint8_t opcode = static_cast<uint8_t>(operation < 8
                            ? 0x80 + operation * 8 + code : (operation == 8 ? 4 : 5) + code * 8);
                        if (!reference.executeOpcode(opcode)) return false;
                        expected.af = reference.registers().af;
                        if (operation >= 8) {
                            setByte(expected, code, prefix, code == 4
                                ? reference.registers().h() : reference.registers().l());
                        }
                        advance(expected, 2, 2);
                        const auto elapsed = run(cpu, {prefix, opcode});
                        if (!elapsed || *elapsed != 8 || !sameRegisters(cpu.registers(), expected)) return false;
                    }
                }
            }
        }
    }
    return true;
}

bool pairs() {
    for (const uint8_t prefix : {0xdd, 0xfd}) {
        for (unsigned value = 0; value < 65536; ++value) {
            for (const uint8_t opcode : {0x21, 0x23, 0x2b, 0xe9, 0xf9}) {
                auto expected = initialRegisters();
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(value);
                index(expected, prefix) = static_cast<uint16_t>(value);
                if (opcode == 0x21) index(expected, prefix) = static_cast<uint16_t>(65535 - value);
                Z80 cpu(expected);
                unsigned cycles = 10;
                if (opcode == 0x21) { index(expected, prefix) = static_cast<uint16_t>(value); cycles = 14; }
                if (opcode == 0x23) index(expected, prefix) = static_cast<uint16_t>(value + 1);
                if (opcode == 0x2b) index(expected, prefix) = static_cast<uint16_t>(value - 1);
                if (opcode == 0xf9) expected.sp = static_cast<uint16_t>(value);
                advance(expected, opcode == 0x21 ? 4 : 2, 2);
                if (opcode == 0xe9) { expected.pc = static_cast<uint16_t>(value); cycles = 8; }
                const auto elapsed = run(cpu, {prefix, opcode, static_cast<uint8_t>(value), static_cast<uint8_t>(value / 256)});
                if (!elapsed || *elapsed != cycles || !sameRegisters(cpu.registers(), expected)) return false;
            }
        }
        for (const uint16_t lhs : {0, 1, 0x0fff, 0x1000, 0x7fff, 0x8000, 0xffff}) {
            for (const uint16_t rhs : {0, 1, 0x0fff, 0x1000, 0x7fff, 0x8000, 0xffff}) {
                for (const uint8_t opcode : {0x09, 0x19, 0x29, 0x39}) {
                    auto expected = initialRegisters();
                    expected.bc = expected.de = expected.sp = rhs;
                    index(expected, prefix) = lhs;
                    Z80 cpu(expected);
                    auto normal = expected;
                    normal.hl = lhs;
                    Z80 reference(normal);
                    if (!reference.executeOpcode(opcode)) return false;
                    index(expected, prefix) = reference.registers().hl;
                    expected.af = reference.registers().af;
                    advance(expected, 2, 2);
                    const auto elapsed = run(cpu, {prefix, opcode});
                    if (!elapsed || *elapsed != 15 || !sameRegisters(cpu.registers(), expected)) return false;
                }
            }
        }
    }
    return true;
}

bool prefixes() {
    const std::vector<std::vector<uint8_t>> instructions{
        {0x00}, {0x06, 0xa5}, {0x01, 0x34, 0x12}, {0x08}, {0xeb}, {0xd9},
        {0xed, 0x4a}, {0xed, 0x57}, {0xed, 0x5f}, {0xed, 0x4f}, {0xed, 0xed},
        {0xc3, 0x34, 0x12}, {0xc2, 0x34, 0x12}, {0x10, 0x80}, {0x18, 0x7f}, {0x20, 0x80},
        {0xf3}, {0xfb}, {0x76}, {0x80}};
    const std::vector<std::vector<uint8_t>> prefixes{{0xdd}, {0xfd}, {0xdd, 0xfd}, {0xfd, 0xdd},
        {0xdd, 0xfd, 0xdd, 0xfd, 0xfd}};
    for (const auto& head : prefixes) {
        for (const auto& instruction : instructions) {
            auto initial = initialRegisters();
            initial.pc = 0xfffd;
            initial.r = 0xfe;
            Z80 cpu(initial);
            advance(initial, static_cast<unsigned>(head.size()), static_cast<unsigned>(head.size()));
            Z80 reference(initial);
            const auto normalTime = reference.executeInstruction(instruction);
            auto bytes = head;
            bytes.insert(bytes.end(), instruction.begin(), instruction.end());
            const auto elapsed = cpu.executeInstruction(bytes);
            if (!normalTime || !elapsed || *elapsed != *normalTime + head.size() * 4
                || !sameRegisters(cpu.registers(), reference.registers())
                || cpu.iff1() != reference.iff1() || cpu.iff2() != reference.iff2()
                || cpu.halted() != reference.halted()
                || cpu.canAcceptMaskableInterrupt() != reference.canAcceptMaskableInterrupt()) return false;
        }
        Z80 cpu(initialRegisters());
        auto expected = cpu.registers();
        auto bytes = head;
        bytes.insert(bytes.end(), {0x21, 0x34, 0x12});
        index(expected, head.back()) = 0x1234;
        advance(expected, static_cast<unsigned>(head.size()) + 3, static_cast<unsigned>(head.size()) + 1);
        const auto elapsed = cpu.executeInstruction(bytes);
        if (!elapsed || *elapsed != 10 + head.size() * 4 || !sameRegisters(cpu.registers(), expected)) return false;
    }
    Z80 enabled(initialRegisters());
    if (!run(enabled, {0xfb}) || enabled.canAcceptMaskableInterrupt()
        || !run(enabled, {0xdd, 0xfd, 0xdd, 0x00}) || !enabled.canAcceptMaskableInterrupt()) return false;
    // A full address-space worth of prefixes must not recurse or truncate the count.
    Z80 longSequence(initialRegisters());
    std::vector<uint8_t> longBytes(65536, 0xdd);
    const auto unchanged = longSequence.registers();
    if (longSequence.executeInstruction(longBytes)
        || !sameRegisters(longSequence.registers(), unchanged)) return false;
    longBytes.back() = 0;
    const auto longTime = longSequence.executeInstruction(longBytes);
    if (!longTime || *longTime != 262144 || !sameRegisters(longSequence.registers(), unchanged)) return false;
    Z80 halted(initialRegisters());
    if (!run(halted, {0xdd, 0xfd, 0x76})) return false;
    auto expected = halted.registers();
    advance(expected, 0, 1);
    const auto idle = run(halted, {0xdd, 0xfd});
    return idle && *idle == 4 && sameRegisters(halted.registers(), expected);
}

bool missing() {
    const std::vector<std::vector<uint8_t>> cases{{}, {0xdd}, {0xfd}, {0xdd, 0xfd},
        {0xdd, 0x21}, {0xdd, 0x21, 0x12}, {0xfd, 0x26}, {0xdd, 0xcb, 0x00},
        {0xfd, 0x34}, {0xdd, 0xed}, {0xdd, 0xed, 0x43}};
    for (const auto& bytes : cases) {
        auto initial = initialRegisters();
        initial.af = 0;
        initial.bc = 0x2828;
        Z80 cpu(initial);
        if (!run(cpu, {0xed, 0x5e}) || !run(cpu, {0xfb}) || !run(cpu, {0xcb, 0x40})) return false;
        const auto before = cpu.registers();
        if (cpu.executeInstruction(bytes) || !sameRegisters(cpu.registers(), before)
            || !cpu.iff1() || !cpu.iff2() || !cpu.canAcceptMaskableInterrupt() || cpu.interruptMode() != 2) return false;
        if (!run(cpu, {0x37}) || (cpu.registers().f() & 0x28) != 0) return false;
    }
    Z80 enabled(initialRegisters());
    return run(enabled, {0xfb}) && !run(enabled, {0xdd, 0x21})
        && !enabled.canAcceptMaskableInterrupt() && run(enabled, {0x00})
        && enabled.canAcceptMaskableInterrupt();
}

bool q() {
    for (const uint8_t prefix : {0xdd, 0xfd}) {
        for (const uint8_t opcode : {0x37, 0x3f}) {
            auto initial = initialRegisters();
            initial.af = 0;
            initial.bc = 0x2828;
            Z80 cpu(initial);
            if (!run(cpu, {0xcb, 0x40}) || !run(cpu, {prefix, opcode})
                || (cpu.registers().f() & 0x28) != 0x28) return false;
            // SCF/CCF writes F itself, so the following unprefixed SCF drops F3/F5.
            if (!run(cpu, {0x37}) || (cpu.registers().f() & 0x28) != 0) return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        bool passed = name == "ld" ? loads() : name == "alu" ? alu() : name == "pair" ? pairs()
            : name == "prefixes" ? prefixes() : name == "missing" ? missing() : name == "q" && q();
        if (passed) return 0;
    }
    std::cerr << "Failed Z80 index test\n";
    return 1;
}
