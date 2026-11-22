#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

uint16_t daaExpected(unsigned a, unsigned f) {
    // Factual correction rows from Young 4.7, evaluated independently of the decoder.
    struct Row { int carry, half; unsigned highMin, highMax, lowMin, lowMax, correction; };
    const std::array<Row, 9> rows{{
        {0, 0, 0, 9, 0, 9, 0x00}, {0, 1, 0, 9, 0, 9, 0x06},
        {0, -1, 0, 8, 10, 15, 0x06}, {0, 0, 10, 15, 0, 9, 0x60},
        {1, 0, 0, 15, 0, 9, 0x60}, {1, 1, 0, 15, 0, 9, 0x66},
        {1, -1, 0, 15, 10, 15, 0x66}, {0, -1, 9, 15, 10, 15, 0x66},
        {0, 1, 10, 15, 0, 9, 0x66},
    }};
    const int carry = f % 2;
    const int half = f / 16 % 2;
    const bool negative = f / 2 % 2 != 0;
    const unsigned high = a / 16;
    const unsigned low = a % 16;
    for (const auto& row : rows) {
        if (carry != row.carry || (row.half != -1 && half != row.half)
            || high < row.highMin || high > row.highMax || low < row.lowMin || low > row.lowMax) {
            continue;
        }
        const unsigned result = (static_cast<int>(a) + 256 + (negative ? -static_cast<int>(row.correction)
            : static_cast<int>(row.correction))) % 256;
        unsigned ones = 0;
        for (unsigned bits = result; bits != 0; bits /= 2) {
            ones += bits % 2;
        }
        const bool newHalf = negative ? half != 0 && low < 6 : low > 9;
        const bool newCarry = carry != 0 || high > 9 || (high == 9 && low > 9);
        const unsigned flags = (result >= 128 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
            + (result / 32 % 2) * 0x20 + (newHalf ? 0x10 : 0) + (result / 8 % 2) * 0x08
            + (ones % 2 == 0 ? 0x04 : 0) + (negative ? 2 : 0) + (newCarry ? 1 : 0);
        return static_cast<uint16_t>(result * 256 + flags);
    }
    return 0xffff; // No row would mean an incomplete test oracle.
}

bool accumulatorOperation(unsigned operation) {
    const std::array<uint8_t, 8> opcodes{0x07, 0x0f, 0x17, 0x1f, 0x2f, 0x37, 0x3f, 0x27};
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned f = 0; f < 256; ++f) {
            auto expected = initialRegisters();
            expected.af = static_cast<uint16_t>(a * 256 + f);
            expected.pc = 0xffff;
            expected.r = static_cast<uint8_t>(f);
            Z80 cpu(expected);
            unsigned result = a;
            unsigned carry = f % 2;
            unsigned h = 0;
            unsigned n = 0;
            if (operation == 0 || operation == 2) {
                carry = a / 128;
                result = a % 128 * 2 + (operation == 0 ? carry : f % 2);
            } else if (operation == 1 || operation == 3) {
                carry = a % 2;
                result = a / 2 + (operation == 1 ? carry : f % 2) * 128;
            } else if (operation == 4) {
                result = 255 - a;
                h = 0x10;
                n = 0x02;
            } else if (operation == 5) {
                carry = 1;
            } else {
                h = (f % 2) * 0x10;
                carry = 1 - f % 2;
            }
            const unsigned xy = operation == 5 || operation == 6 ? result | f : result;
            const unsigned flags = (f / 128 % 2) * 0x80 + (f / 64 % 2) * 0x40
                + (xy / 32 % 2) * 0x20 + h + (xy / 8 % 2) * 0x08
                + (f / 4 % 2) * 0x04 + n + carry;
            expected.af = operation == 7 ? daaExpected(a, f)
                : static_cast<uint16_t>(result * 256 + flags);
            expected.pc = 0;
            expected.r = static_cast<uint8_t>(f == 127 ? 0 : f == 255 ? 128 : f + 1);
            const auto elapsed = cpu.executeOpcode(opcodes[operation]);
            if (!elapsed || *elapsed != 4 || !sameRegisters(cpu.registers(), expected)) {
                std::cerr << "A=" << a << " initial F=" << f << '\n';
                return false;
            }
        }
    }
    return true;
}

bool bcdArithmetic() {
    for (const bool subtract : {false, true}) {
        for (int a = 0; a < 100; ++a) {
            for (int b = 0; b < 100; ++b) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>((a / 10 * 16 + a % 10) * 256);
                Z80 cpu(expected);
                const auto first = cpu.executeOpcode(subtract ? 0xd6 : 0xc6,
                    static_cast<uint8_t>(b / 10 * 16 + b % 10));
                const auto second = cpu.executeOpcode(0x27);
                const int decimal = subtract ? a - b : a + b;
                const int wrapped = (decimal + 100) % 100;
                const unsigned packed = wrapped / 10 * 16 + wrapped % 10;
                if (!first || !second || *first + *second != 11
                    || cpu.registers().a() != packed
                    || (cpu.registers().f() & 3) != (subtract ? 2 : 0) + (decimal < 0 || decimal > 99 ? 1 : 0)) {
                    std::cerr << "BCD subtract=" << subtract << " a=" << a << " b=" << b << '\n';
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::array<std::string_view, 8> names{"rlca", "rrca", "rla", "rra", "cpl", "scf", "ccf", "daa"};
    if (argc == 2) {
        if (std::string_view(argv[1]) == "bcd") {
            return bcdArithmetic() ? 0 : 1;
        }
        for (unsigned operation = 0; operation < names.size(); ++operation) {
            if (argv[1] == names[operation]) {
                return accumulatorOperation(operation) ? 0 : 1;
            }
        }
    }
    std::cerr << "Unknown accumulator test\n";
    return 1;
}
