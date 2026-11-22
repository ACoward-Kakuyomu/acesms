#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

bool rotateShift(unsigned operation) {
    const std::array<uint8_t, 7> codes{0, 1, 2, 3, 4, 5, 7};
    const std::array<uint16_t Z80Registers::*, 7> pairs{
        &Z80Registers::bc, &Z80Registers::bc, &Z80Registers::de, &Z80Registers::de,
        &Z80Registers::hl, &Z80Registers::hl, &Z80Registers::af};
    for (size_t target = 0; target < codes.size(); ++target) {
        for (unsigned input = 0; input < 256; ++input) {
            for (const unsigned flags : {0u, 1u, 254u, 255u}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + flags);
                auto& pair = expected.*pairs[target];
                pair = static_cast<uint16_t>(target % 2 == 0
                    ? input * 256 + pair % 256 : pair / 256 * 256 + input);
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(input);
                Z80 cpu(expected);
                unsigned result;
                switch (operation) {
                case 0: result = input % 128 * 2 + input / 128; break;
                case 1: result = input / 2 + input % 2 * 128; break;
                case 2: result = input % 128 * 2 + flags % 2; break;
                case 3: result = input / 2 + flags % 2 * 128; break;
                case 4: result = input % 128 * 2; break;
                case 5: result = input / 2 + input / 128 * 128; break;
                case 6: result = input % 128 * 2 + 1; break;
                default: result = input / 2; break;
                }
                unsigned ones = 0;
                for (unsigned bits = result; bits != 0; bits /= 2) ones += bits % 2;
                const unsigned carry = operation % 2 == 0 ? input / 128 : input % 2;
                const unsigned resultFlags = (result >= 128 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
                    + (result / 32 % 2) * 0x20 + (result / 8 % 2) * 0x08
                    + (ones % 2 == 0 ? 0x04 : 0) + carry;
                pair = static_cast<uint16_t>(target % 2 == 0
                    ? result * 256 + pair % 256 : pair / 256 * 256 + result);
                expected.af = static_cast<uint16_t>(expected.af / 256 * 256 + resultFlags);
                expected.pc = 1;
                expected.r = static_cast<uint8_t>(input / 128 * 128 + (input % 128 + 2) % 128);
                const auto elapsed = cpu.executeOpcode(0xcb, static_cast<uint8_t>(operation * 8 + codes[target]));
                if (!elapsed || *elapsed != 8 || !sameRegisters(cpu.registers(), expected)) {
                    std::cerr << "CB operation=" << operation << " target=" << target
                              << " input=" << input << " F=" << flags << '\n';
                    return false;
                }
            }
        }
    }
    return true;
}

bool missingAndMemory() {
    Z80 cpu(initialRegisters());
    if (cpu.executeOpcode(0xcb)) return false;
    for (unsigned operation = 0; operation < 32; ++operation) {
        if (cpu.executeOpcode(0xcb, static_cast<uint8_t>(operation * 8 + 6))) return false;
    }
    return sameRegisters(cpu.registers(), initialRegisters());
}

bool bitOperation(unsigned group) {
    const std::array<uint8_t, 7> codes{0, 1, 2, 3, 4, 5, 7};
    const std::array<uint16_t Z80Registers::*, 7> pairs{
        &Z80Registers::bc, &Z80Registers::bc, &Z80Registers::de, &Z80Registers::de,
        &Z80Registers::hl, &Z80Registers::hl, &Z80Registers::af};
    for (size_t target = 0; target < codes.size(); ++target) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            const unsigned power = 1u << bit;
            for (unsigned input = 0; input < 256; ++input) {
                for (const unsigned flags : {0u, 1u, 254u, 255u}) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    auto& pair = expected.*pairs[target];
                    pair = static_cast<uint16_t>(target % 2 == 0
                        ? input * 256 + pair % 256 : pair / 256 * 256 + input);
                    expected.pc = 0xffff;
                    expected.r = static_cast<uint8_t>(input);
                    Z80 cpu(expected);
                    const bool set = input / power % 2 != 0;
                    unsigned resultFlags = flags;
                    unsigned result = input;
                    if (group == 1) {
                        resultFlags = flags % 2 + 0x10 + (!set ? 0x44 : 0)
                            + (bit == 7 && set ? 0x80 : 0)
                            + (input / 32 % 2) * 0x20 + (input / 8 % 2) * 0x08;
                    } else {
                        result = group == 2 ? input - (set ? power : 0) : input + (set ? 0 : power);
                    }
                    pair = static_cast<uint16_t>(target % 2 == 0
                        ? result * 256 + pair % 256 : pair / 256 * 256 + result);
                    expected.af = static_cast<uint16_t>(expected.af / 256 * 256 + resultFlags);
                    expected.pc = 1;
                    expected.r = static_cast<uint8_t>(input / 128 * 128 + (input % 128 + 2) % 128);
                    const auto elapsed = cpu.executeOpcode(0xcb,
                        static_cast<uint8_t>(group * 64 + bit * 8 + codes[target]));
                    if (!elapsed || *elapsed != 8 || !sameRegisters(cpu.registers(), expected)) {
                        std::cerr << "CB bit group=" << group << " bit=" << bit
                                  << " target=" << target << " input=" << input << '\n';
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool bitFlagSource() {
    auto initial = initialRegisters();
    initial.bc = 0x2845;
    initial.af = 0x1200;
    Z80 cpu(initial);
    const auto elapsed = cpu.executeOpcode(0xcb, 0x40); // BIT 0,B.
    initial.af = 0x127c;
    initial.pc += 2;
    initial.r += 2;
    return elapsed && *elapsed == 8 && sameRegisters(cpu.registers(), initial);
}

} // namespace

int main(int argc, char* argv[]) {
    const std::array<std::string_view, 8> names{"rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"};
    if (argc == 2) {
        if (std::string_view(argv[1]) == "missing") return missingAndMemory() ? 0 : 1;
        if (std::string_view(argv[1]) == "bit") return bitOperation(1) ? 0 : 1;
        if (std::string_view(argv[1]) == "res") return bitOperation(2) ? 0 : 1;
        if (std::string_view(argv[1]) == "set") return bitOperation(3) ? 0 : 1;
        if (std::string_view(argv[1]) == "bit_flags") return bitFlagSource() ? 0 : 1;
        for (unsigned operation = 0; operation < names.size(); ++operation) {
            if (argv[1] == names[operation]) return rotateShift(operation) ? 0 : 1;
        }
    }
    std::cerr << "Unknown CB test\n";
    return 1;
}
