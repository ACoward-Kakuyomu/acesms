#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

uint16_t arithmeticResult(unsigned operation, int a, int b, unsigned initialFlags) {
    if (operation >= 4 && operation <= 6) {
        const int result = operation == 4 ? a & b : operation == 5 ? a ^ b : a | b;
        unsigned ones = 0;
        for (int bit = 0; bit < 8; ++bit) {
            ones += (result >> bit) % 2;
        }
        const unsigned flags = (result >= 128 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
            + (result / 32 % 2) * 0x20 + (operation == 4 ? 0x10 : 0)
            + (result / 8 % 2) * 0x08 + (ones % 2 == 0 ? 0x04 : 0);
        return static_cast<uint16_t>(result * 256 + flags);
    }
    const bool subtract = operation >= 2;
    const int carry = operation == 1 || operation == 3 ? initialFlags % 2 : 0;
    const int wide = subtract ? a - b - carry : a + b + carry;
    const int result = (wide + 256) % 256;
    const int signedA = a - (a < 128 ? 0 : 256);
    const int signedB = b - (b < 128 ? 0 : 256);
    const int signedWide = subtract ? signedA - signedB - carry : signedA + signedB + carry;
    const int low = subtract ? a % 16 - b % 16 - carry : a % 16 + b % 16 + carry;
    const int xy = operation == 7 ? b : result;
    const unsigned flags = (result >= 128 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
        + (xy / 32 % 2) * 0x20 + (low < 0 || low > 15 ? 0x10 : 0)
        + (xy / 8 % 2) * 0x08 + (signedWide < -128 || signedWide > 127 ? 0x04 : 0)
        + (subtract ? 0x02 : 0) + (wide < 0 || wide > 255 ? 1 : 0);
    return static_cast<uint16_t>((operation == 7 ? a : result) * 256 + flags);
}

bool arithmetic(unsigned operation) {
    const std::array<uint8_t, 8> immediates{0xc6, 0xce, 0xd6, 0xde, 0xe6, 0xee, 0xf6, 0xfe};
    const std::array<uint8_t, 8> registerBases{0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0, 0xb8};
    for (int a = 0; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            for (const unsigned flags : {0xfeu, 0xffu}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(a * 256 + flags);
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(b);
                Z80 cpu(expected);
                expected.af = arithmeticResult(operation, a, b, flags);
                expected.pc = 1;
                expected.r = static_cast<uint8_t>(b == 127 ? 0 : b == 255 ? 128 : b + 1);
                const auto elapsed = cpu.executeOpcode(immediates[operation], static_cast<uint8_t>(b));
                if (!elapsed || *elapsed != 7 || !sameRegisters(cpu.registers(), expected)) {
                    std::cerr << "Immediate a=" << a << " b=" << b << " F=" << flags << '\n';
                    return false;
                }
            }
        }
    }
    // Each register must select its own byte; A uses the same pre-operation value twice.
    for (const uint8_t code : {0, 1, 2, 3, 4, 5, 7}) {
        for (unsigned a = 0; a < 256; ++a) {
            const std::array<unsigned, 8> operands{0x23, 0x45, 0x34, 0x56, 0xab, 0xcd, 0, a};
            auto expected = initialRegisters();
            expected.hl = 0xabcd;
            expected.af = static_cast<uint16_t>(a * 256 + (a % 2));
            expected.pc = 0xffff;
            expected.r = 0xff;
            Z80 cpu(expected);
            expected.af = arithmeticResult(operation, a, operands[code], a % 2);
            expected.pc = 0;
            expected.r = 0x80;
            const auto elapsed = cpu.executeOpcode(static_cast<uint8_t>(registerBases[operation] + code));
            if (!elapsed || *elapsed != 4 || !sameRegisters(cpu.registers(), expected)) {
                std::cerr << "Register code=" << unsigned(code) << " a=" << a << '\n';
                return false;
            }
        }
    }
    Z80 cpu(initialRegisters());
    return !cpu.executeOpcode(immediates[operation])
        && !cpu.executeOpcode(static_cast<uint8_t>(registerBases[operation] + 6))
        && sameRegisters(cpu.registers(), initialRegisters());
}

} // namespace

int main(int argc, char* argv[]) {
    const std::array<std::string_view, 8> names{"add", "adc", "sub", "sbc", "and", "xor", "or", "cp"};
    if (argc == 2) {
        for (unsigned operation = 0; operation < names.size(); ++operation) {
            if (argv[1] == names[operation]) {
                return arithmetic(operation) ? 0 : 1;
            }
        }
    }
    std::cerr << "Unknown ALU test\n";
    return 1;
}
