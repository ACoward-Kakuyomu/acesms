#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>
#include <utility>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

bool pairOperation(unsigned operation) {
    const std::array<uint16_t Z80Registers::*, 4> pairs{
        &Z80Registers::bc, &Z80Registers::de, &Z80Registers::hl, &Z80Registers::sp};
    const std::array<std::array<uint8_t, 4>, 3> opcodes{{
        {0x01, 0x11, 0x21, 0x31}, {0x03, 0x13, 0x23, 0x33}, {0x0b, 0x1b, 0x2b, 0x3b}}};
    for (size_t target = 0; target < pairs.size(); ++target) {
        for (unsigned value = 0; value < 65536; ++value) {
            auto expected = initialRegisters();
            expected.af = static_cast<uint16_t>(0x1200 + value % 256);
            expected.pc = 0xffff;
            expected.r = static_cast<uint8_t>(value);
            if (operation != 0) {
                expected.*pairs[target] = static_cast<uint16_t>(value);
            }
            Z80 cpu(expected);
            expected.*pairs[target] = static_cast<uint16_t>(operation == 0 ? value
                : operation == 1 ? (value + 1) % 65536 : (value + 65535) % 65536);
            expected.pc = operation == 0 ? 2 : 0;
            const unsigned refresh = value % 256;
            expected.r = static_cast<uint8_t>(refresh == 127 ? 0 : refresh == 255 ? 128 : refresh + 1);
            const auto elapsed = operation == 0
                ? cpu.executeOpcode(opcodes[operation][target], static_cast<uint8_t>(value),
                    static_cast<uint8_t>(value / 256))
                : cpu.executeOpcode(opcodes[operation][target]);
            if (!elapsed || *elapsed != (operation == 0 ? 10 : 6)
                || !sameRegisters(cpu.registers(), expected)) {
                std::cerr << "Pair target=" << target << " value=" << value << '\n';
                return false;
            }
        }
        if (operation == 0) {
            Z80 cpu(initialRegisters());
            const auto opcode = opcodes[0][target];
            if (cpu.executeOpcode(opcode) || cpu.executeOpcode(opcode, 0x34)
                || cpu.executeOpcode(opcode, std::nullopt, 0x12)
                || !sameRegisters(cpu.registers(), initialRegisters())) {
                return false;
            }
        }
    }
    return true;
}

bool exchanges() {
    for (const uint8_t opcode : {0x08, 0xd9, 0xeb}) {
        for (unsigned flags = 0; flags < 256; ++flags) {
            auto expected = initialRegisters();
            expected.af = static_cast<uint16_t>(0x1200 + flags);
            expected.pc = 0xffff;
            expected.r = 0x7f;
            const auto original = expected;
            Z80 cpu(expected);
            if (opcode == 0x08) {
                std::swap(expected.af, expected.afAlternate);
            } else if (opcode == 0xd9) {
                std::swap(expected.bc, expected.bcAlternate);
                std::swap(expected.de, expected.deAlternate);
                std::swap(expected.hl, expected.hlAlternate);
            } else {
                std::swap(expected.de, expected.hl);
            }
            expected.pc = 0;
            expected.r = 0;
            const auto first = cpu.executeOpcode(opcode);
            if (!first || *first != 4 || !sameRegisters(cpu.registers(), expected)) {
                return false;
            }
            expected = original;
            expected.pc = 1;
            expected.r = 1;
            const auto second = cpu.executeOpcode(opcode);
            if (!second || *second != 4 || !sameRegisters(cpu.registers(), expected)) {
                return false;
            }
        }
    }
    return true;
}

bool loadSp() {
    for (unsigned value = 0; value < 65536; ++value) {
        auto expected = initialRegisters();
        expected.hl = static_cast<uint16_t>(value);
        expected.af = static_cast<uint16_t>(0x1200 + value % 256);
        Z80 cpu(expected);
        expected.sp = static_cast<uint16_t>(value);
        ++expected.pc;
        ++expected.r;
        const auto elapsed = cpu.executeOpcode(0xf9);
        if (!elapsed || *elapsed != 6 || !sameRegisters(cpu.registers(), expected)) {
            return false;
        }
    }
    return true;
}

bool addHl() {
    const std::array<uint16_t Z80Registers::*, 4> pairs{
        &Z80Registers::bc, &Z80Registers::de, &Z80Registers::hl, &Z80Registers::sp};
    const std::array<uint8_t, 4> opcodes{0x09, 0x19, 0x29, 0x39};
    for (size_t source = 0; source < pairs.size(); ++source) {
        for (unsigned hl = 0; hl < 65536; ++hl) {
            for (const unsigned operand : {0u, 1u, 0x0fffu, 0x1000u, 0x7fffu, 0x8000u, 0xffffu, 0xaaaau, 0x5555u}) {
                if (source == 2 && operand != 0) {
                    continue; // ADD HL,HL has no independent second operand.
                }
                for (const unsigned flags : {0u, 255u}) {
                    auto expected = initialRegisters();
                    expected.*pairs[source] = static_cast<uint16_t>(operand);
                    expected.hl = static_cast<uint16_t>(hl);
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.pc = 0xffff;
                    expected.r = 0xff;
                    Z80 cpu(expected);
                    const unsigned rhs = source == 2 ? hl : operand;
                    const unsigned wide = hl + rhs;
                    const unsigned result = wide % 65536;
                    const unsigned high = result / 256;
                    const unsigned expectedFlags = (flags / 128 % 2) * 0x80
                        + (flags / 64 % 2) * 0x40 + (high / 32 % 2) * 0x20
                        + (hl % 4096 + rhs % 4096 > 4095 ? 0x10 : 0)
                        + (high / 8 % 2) * 0x08 + (flags / 4 % 2) * 0x04
                        + (wide > 65535 ? 1 : 0);
                    expected.hl = static_cast<uint16_t>(result);
                    expected.af = static_cast<uint16_t>(0x1200 + expectedFlags);
                    expected.pc = 0;
                    expected.r = 0x80;
                    const auto elapsed = cpu.executeOpcode(opcodes[source]);
                    if (!elapsed || *elapsed != 11 || !sameRegisters(cpu.registers(), expected)) {
                        std::cerr << "ADD HL source=" << source << " hl=" << hl
                                  << " rhs=" << rhs << " F=" << flags << '\n';
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "load") return pairOperation(0) ? 0 : 1;
        if (name == "inc") return pairOperation(1) ? 0 : 1;
        if (name == "dec") return pairOperation(2) ? 0 : 1;
        if (name == "exchange") return exchanges() ? 0 : 1;
        if (name == "sp") return loadSp() ? 0 : 1;
        if (name == "add") return addHl() ? 0 : 1;
    }
    std::cerr << "Unknown pair test\n";
    return 1;
}
