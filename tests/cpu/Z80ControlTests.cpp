#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

bool enableInterrupts() {
    Z80 cpu(initialRegisters());
    if (cpu.iff1() || cpu.iff2() || cpu.canAcceptMaskableInterrupt() || cpu.halted()) return false;
    auto elapsed = cpu.executeOpcode(0xfb);
    if (!elapsed || *elapsed != 4 || !cpu.iff1() || !cpu.iff2() || cpu.canAcceptMaskableInterrupt()) return false;
    const auto afterEi = cpu.registers();
    if (cpu.executeOpcode(0xcb) || !sameRegisters(cpu.registers(), afterEi)
        || cpu.canAcceptMaskableInterrupt()) return false;
    elapsed = cpu.executeOpcode(0x00);
    if (!elapsed || *elapsed != 4 || !cpu.canAcceptMaskableInterrupt()) return false;
    elapsed = cpu.executeOpcode(0xfb);
    if (!elapsed || cpu.canAcceptMaskableInterrupt()) return false;
    elapsed = cpu.executeOpcode(0xfb);
    if (!elapsed || cpu.canAcceptMaskableInterrupt()) return false;
    elapsed = cpu.executeOpcode(0xcb, 0x00);
    if (!elapsed || *elapsed != 8 || !cpu.canAcceptMaskableInterrupt()) return false;
    elapsed = cpu.executeOpcode(0xfb);
    if (!elapsed || cpu.canAcceptMaskableInterrupt()) return false;
    elapsed = cpu.executeOpcode(0xed, 0x44);
    return elapsed && *elapsed == 8 && cpu.canAcceptMaskableInterrupt() && cpu.iff1() && cpu.iff2();
}

bool disableInterrupts() {
    Z80 cpu(initialRegisters());
    if (!cpu.executeOpcode(0xfb)) return false;
    const auto elapsed = cpu.executeOpcode(0xf3);
    if (!elapsed || *elapsed != 4 || cpu.iff1() || cpu.iff2() || cpu.canAcceptMaskableInterrupt()) return false;
    if (!cpu.executeOpcode(0x00) || cpu.canAcceptMaskableInterrupt()) return false;
    if (!cpu.executeOpcode(0xfb) || !cpu.executeOpcode(0x00) || !cpu.canAcceptMaskableInterrupt()) return false;
    if (!cpu.executeOpcode(0xf3) || cpu.canAcceptMaskableInterrupt()) return false;
    return cpu.registers().af == initialRegisters().af;
}

bool haltCycles() {
    for (unsigned refresh = 0; refresh < 256; ++refresh) {
        auto expected = initialRegisters();
        expected.pc = 0xffff;
        expected.r = static_cast<uint8_t>(refresh);
        Z80 cpu(expected);
        const auto first = cpu.executeOpcode(0x76);
        expected.pc = 0;
        expected.r = static_cast<uint8_t>(refresh / 128 * 128 + (refresh % 128 + 1) % 128);
        if (!first || *first != 4 || !cpu.halted() || !sameRegisters(cpu.registers(), expected)) return false;
        for (const uint8_t supplied : {0x04, 0x76, 0xcb, 0xfb, 0x00}) {
            const auto elapsed = cpu.executeOpcode(supplied);
            const unsigned low = expected.r % 128;
            expected.r = static_cast<uint8_t>(expected.r / 128 * 128 + (low + 1) % 128);
            if (!elapsed || *elapsed != 4 || !cpu.halted() || cpu.iff1() || cpu.iff2()
                || !sameRegisters(cpu.registers(), expected)) return false;
        }
    }
    Z80 enabled(initialRegisters());
    if (!enabled.executeOpcode(0xfb) || !enabled.executeOpcode(0x76)) return false;
    return enabled.halted() && enabled.canAcceptMaskableInterrupt();
}

bool resetState() {
    auto initial = initialRegisters();
    Z80 cpu(initial);
    if (!cpu.executeOpcode(0xfb) || !cpu.executeOpcode(0x76)) return false;
    cpu.reset();
    initial.pc = 0;
    initial.i = 0;
    initial.r = 0;
    if (!sameRegisters(cpu.registers(), initial) || cpu.iff1() || cpu.iff2()
        || cpu.halted() || cpu.canAcceptMaskableInterrupt()) return false;
    cpu.reset();
    if (!sameRegisters(cpu.registers(), initial)) return false;
    const auto elapsed = cpu.executeOpcode(0x00);
    initial.pc = 1;
    initial.r = 1;
    return elapsed && *elapsed == 4 && sameRegisters(cpu.registers(), initial);
}

bool qSequences() {
    struct Previous { uint8_t opcode; std::optional<uint8_t> operand; bool writesFlags; };
    const std::array<Previous, 20> previous{{
        {0x00, std::nullopt, false}, {0x06, 0xa5, false}, {0x03, std::nullopt, false},
        {0x08, std::nullopt, false}, {0xd9, std::nullopt, false}, {0xe9, std::nullopt, false},
        {0xf3, std::nullopt, false}, {0xfb, std::nullopt, false}, {0x04, std::nullopt, true},
        {0x80, std::nullopt, true}, {0x27, std::nullopt, true}, {0x09, std::nullopt, true},
        {0x37, std::nullopt, true}, {0x3f, std::nullopt, true},
        {0xcb, 0x00, true}, {0xcb, 0x40, true}, {0xcb, 0x80, false}, {0xcb, 0xc0, false},
        {0xed, 0x44, true}, {0xed, 0x4a, true},
    }};
    for (const auto& prior : previous) {
        for (const uint8_t opcode : {0x37, 0x3f}) {
            for (unsigned a = 0; a < 256; ++a) {
                for (unsigned flags = 0; flags < 256; ++flags) {
                    auto initial = initialRegisters();
                    initial.af = static_cast<uint16_t>(a * 256 + flags);
                    Z80 cpu(initial);
                    if (!cpu.executeOpcode(prior.opcode, prior.operand)) return false;
                    auto expected = cpu.registers();
                    if (cpu.executeOpcode(0xcb) || !sameRegisters(cpu.registers(), expected)) return false;
                    const unsigned oldF = expected.f();
                    const unsigned xy = expected.a() | (prior.writesFlags ? 0 : oldF);
                    const unsigned cf = oldF % 2;
                    const unsigned newF = (oldF / 128 % 2) * 0x80 + (oldF / 64 % 2) * 0x40
                        + (xy / 32 % 2) * 0x20 + (xy / 8 % 2) * 0x08 + (oldF / 4 % 2) * 0x04
                        + (opcode == 0x37 ? 1 : (cf != 0 ? 0x10 : 1));
                    expected.af = static_cast<uint16_t>(expected.a() * 256 + newF);
                    ++expected.pc;
                    const unsigned refresh = expected.r;
                    expected.r = static_cast<uint8_t>(refresh / 128 * 128 + (refresh % 128 + 1) % 128);
                    const auto elapsed = cpu.executeOpcode(opcode);
                    if (!elapsed || *elapsed != 4 || !sameRegisters(cpu.registers(), expected)) {
                        std::cerr << "Q previous=" << unsigned(prior.opcode) << " next=" << unsigned(opcode)
                                  << " A=" << a << " F=" << flags << '\n';
                        return false;
                    }
                }
            }
        }
    }
    // Reset clears history even though general-purpose registers (including F) survive.
    Z80 cpu(initialRegisters());
    if (!cpu.executeOpcode(0x04)) return false;
    cpu.reset();
    const unsigned xy = cpu.registers().f() | cpu.registers().a();
    if (!cpu.executeOpcode(0x37)) return false;
    return (cpu.registers().f() & 0x28) == (xy & 0x28);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "ei") return enableInterrupts() ? 0 : 1;
        if (name == "di") return disableInterrupts() ? 0 : 1;
        if (name == "halt") return haltCycles() ? 0 : 1;
        if (name == "reset") return resetState() ? 0 : 1;
        if (name == "q") return qSequences() ? 0 : 1;
    }
    std::cerr << "Unknown control test\n";
    return 1;
}
