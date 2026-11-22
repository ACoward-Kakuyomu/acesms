#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

// Rows and columns: B, C, D, E, H, L, A (Zilog LD r,r' table).
constexpr std::array<std::array<uint8_t, 7>, 7> ldOpcodes{{
    {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x47},
    {0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4f},
    {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x57},
    {0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5f},
    {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67},
    {0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6f},
    {0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7f},
}};

constexpr std::array<uint8_t, 7> immediateOpcodes{
    0x06, 0x0e, 0x16, 0x1e, 0x26, 0x2e, 0x3e}; // B, C, D, E, H, L, A

constexpr std::array<uint8_t, 7> incOpcodes{0x04, 0x0c, 0x14, 0x1c, 0x24, 0x2c, 0x3c};
constexpr std::array<uint8_t, 7> decOpcodes{0x05, 0x0d, 0x15, 0x1d, 0x25, 0x2d, 0x3d};

bool registersAreOwned() {
    auto initial = initialRegisters();
    const Z80 cpu(initial);
    initial.af = 0;
    initial.pc = 0;
    const Z80 other(initial);
    return sameRegisters(cpu.registers(), initialRegisters())
        && sameRegisters(other.registers(), initial);
}

bool registerBytesMatchPairs() {
    struct Access {
        uint8_t (Z80Registers::*get)() const;
        void (Z80Registers::*set)(uint8_t);
        uint16_t Z80Registers::*pair;
        bool high;
    };
    const std::array<Access, 8> access{{
        {&Z80Registers::a, &Z80Registers::setA, &Z80Registers::af, true},
        {&Z80Registers::f, &Z80Registers::setF, &Z80Registers::af, false},
        {&Z80Registers::b, &Z80Registers::setB, &Z80Registers::bc, true},
        {&Z80Registers::c, &Z80Registers::setC, &Z80Registers::bc, false},
        {&Z80Registers::d, &Z80Registers::setD, &Z80Registers::de, true},
        {&Z80Registers::e, &Z80Registers::setE, &Z80Registers::de, false},
        {&Z80Registers::h, &Z80Registers::setH, &Z80Registers::hl, true},
        {&Z80Registers::l, &Z80Registers::setL, &Z80Registers::hl, false},
    }};
    for (const auto& byte : access) {
        for (unsigned value = 0; value < 256; ++value) {
            auto actual = initialRegisters();
            auto expected = actual;
            (actual.*byte.set)(static_cast<uint8_t>(value));
            const auto oldPair = expected.*byte.pair;
            expected.*byte.pair = static_cast<uint16_t>(byte.high
                ? value * 256 + oldPair % 256
                : oldPair / 256 * 256 + value);
            if (!sameRegisters(actual, expected)
                || (actual.*byte.get)() != value) {
                return false;
            }
            // A direct pair write must also be visible through the byte getter.
            const auto replacement = 255 - value;
            actual.*byte.pair = static_cast<uint16_t>(
                byte.high ? replacement * 256 + 0xaa : 0x5500 + replacement);
            if ((actual.*byte.get)() != replacement) {
                return false;
            }
        }
    }
    return true;
}

bool nopPreservesRegisters() {
    for (const uint16_t flags : {0x00, 0xff, 0x55, 0xaa}) {
        auto expected = initialRegisters();
        expected.af = static_cast<uint16_t>(0x1200 | flags);
        Z80 cpu(expected);
        const Z80 other(expected);
        for (int instruction = 0; instruction < 3; ++instruction) {
            const auto elapsed = cpu.executeOpcode(0x00);
            ++expected.pc;
            ++expected.r;
            if (!elapsed || *elapsed != 4
                || !sameRegisters(cpu.registers(), expected)) {
                return false;
            }
        }
        auto unchanged = initialRegisters();
        unchanged.af = static_cast<uint16_t>(0x1200 | flags);
        if (!sameRegisters(other.registers(), unchanged)) {
            return false;
        }
    }
    return true;
}

bool nopWrapsPcAndRefresh() {
    for (unsigned refresh = 0; refresh < 256; ++refresh) {
        auto expected = initialRegisters();
        expected.pc = 0xffff;
        expected.r = static_cast<uint8_t>(refresh);
        Z80 cpu(expected);
        const auto elapsed = cpu.executeOpcode(0x00);
        expected.pc = 0;
        // Independent boundary expectation: only 7F and FF wrap the low bits.
        expected.r = static_cast<uint8_t>(
            refresh == 0x7f ? 0 : refresh == 0xff ? 0x80 : refresh + 1);
        if (!elapsed || *elapsed != 4
            || !sameRegisters(cpu.registers(), expected)) {
            return false;
        }
    }
    return true;
}

bool unsupportedOpcodesPreserveState() {
    Z80 cpu(initialRegisters());
    for (unsigned opcode = 1; opcode < 256; ++opcode) {
        bool supported = false;
        for (const auto& row : ldOpcodes) {
            for (const auto ld : row) {
                supported = supported || opcode == ld;
            }
        }
        for (const auto inc : incOpcodes) {
            supported = supported || opcode == inc;
        }
        for (const auto dec : decOpcodes) {
            supported = supported || opcode == dec;
        }
        for (const auto accumulator : {0x07, 0x0f, 0x17, 0x1f, 0x27, 0x2f, 0x37, 0x3f}) {
            supported = supported || opcode == accumulator;
        }
        for (const auto pair : {0x03, 0x13, 0x23, 0x33, 0x0b, 0x1b, 0x2b, 0x3b,
                               0x08, 0xd9, 0xeb, 0xf9, 0x09, 0x19, 0x29, 0x39}) {
            supported = supported || opcode == pair;
        }
        // Eight ALU operations, register operands only (exclude (HL)).
        supported = supported || opcode == 0xe9;
        supported = supported || opcode == 0x76 || opcode == 0xf3 || opcode == 0xfb;
        // Conditional RET can execute without memory when its condition is false.
        for (const auto ret : {0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8}) {
            supported = supported || opcode == ret;
        }
        if (opcode >= 0x80 && opcode <= 0xbf && opcode % 8 != 6) {
            supported = true;
        }
        if (supported) {
            continue;
        }
        if (cpu.executeOpcode(static_cast<uint8_t>(opcode)).has_value()
            || !sameRegisters(cpu.registers(), initialRegisters())) {
            return false;
        }
        bool immediateSupported = false;
        for (const auto immediate : immediateOpcodes) {
            immediateSupported = immediateSupported || opcode == immediate;
        }
        immediateSupported = immediateSupported || opcode == 0xc6 || opcode == 0xce
            || opcode == 0xd6 || opcode == 0xde || opcode == 0xe6 || opcode == 0xee
            || opcode == 0xf6 || opcode == 0xfe;
        for (const auto relative : {0x10, 0x18, 0x20, 0x28, 0x30, 0x38}) {
            immediateSupported = immediateSupported || opcode == relative;
        }
        immediateSupported = immediateSupported || opcode == 0xcb || opcode == 0xed
            || opcode == 0xdd || opcode == 0xfd;
        if (!immediateSupported
            && (cpu.executeOpcode(static_cast<uint8_t>(opcode), 0xa5).has_value()
                || !sameRegisters(cpu.registers(), initialRegisters()))) {
            return false;
        }
    }
    return true;
}

bool ldCopiesRegisterBytes() {
    struct Byte {
        uint16_t Z80Registers::*pair;
        bool high;
    };
    const std::array<Byte, 7> bytes{{
        {&Z80Registers::bc, true}, {&Z80Registers::bc, false},
        {&Z80Registers::de, true}, {&Z80Registers::de, false},
        {&Z80Registers::hl, true}, {&Z80Registers::hl, false},
        {&Z80Registers::af, true},
    }};
    for (size_t destination = 0; destination < bytes.size(); ++destination) {
        for (size_t source = 0; source < bytes.size(); ++source) {
            for (unsigned value = 0; value < 256; ++value) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0xef00 + value);
                expected.bc = 0x2345;
                expected.de = 0x6789;
                expected.hl = 0xabcd;
                expected.pc = value % 2 == 0 ? 0x1234 : 0xffff;
                expected.r = static_cast<uint8_t>(value);
                auto& sourcePair = expected.*bytes[source].pair;
                sourcePair = static_cast<uint16_t>(bytes[source].high
                    ? value * 256 + sourcePair % 256
                    : sourcePair / 256 * 256 + value);
                Z80 cpu(expected);

                auto& destinationPair = expected.*bytes[destination].pair;
                destinationPair = static_cast<uint16_t>(bytes[destination].high
                    ? value * 256 + destinationPair % 256
                    : destinationPair / 256 * 256 + value);
                expected.pc = value % 2 == 0 ? 0x1235 : 0;
                expected.r = static_cast<uint8_t>(
                    value == 0x7f ? 0 : value == 0xff ? 0x80 : value + 1);

                const auto elapsed = cpu.executeOpcode(ldOpcodes[destination][source]);
                if (!elapsed || *elapsed != 4
                    || !sameRegisters(cpu.registers(), expected)) {
                    std::cerr << "LD destination=" << destination << " source="
                              << source << " value=" << value << '\n';
                    return false;
                }
            }
        }
    }
    return true;
}

bool ldImmediateWritesRegister() {
    const std::array<uint16_t Z80Registers::*, 7> pairs{
        &Z80Registers::bc, &Z80Registers::bc,
        &Z80Registers::de, &Z80Registers::de,
        &Z80Registers::hl, &Z80Registers::hl, &Z80Registers::af};
    for (size_t destination = 0; destination < pairs.size(); ++destination) {
        for (unsigned value = 0; value < 256; ++value) {
            for (const uint16_t pc : {0x1234, 0xfffe, 0xffff}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + (255 - value));
                expected.pc = pc;
                expected.r = static_cast<uint8_t>(value);
                Z80 cpu(expected);

                auto& pair = expected.*pairs[destination];
                pair = static_cast<uint16_t>(destination % 2 == 0
                    ? value * 256 + pair % 256
                    : pair / 256 * 256 + value);
                expected.pc = pc == 0x1234 ? 0x1236 : pc == 0xfffe ? 0 : 1;
                expected.r = static_cast<uint8_t>(
                    value == 0x7f ? 0 : value == 0xff ? 0x80 : value + 1);

                const auto elapsed = cpu.executeOpcode(
                    immediateOpcodes[destination], static_cast<uint8_t>(value));
                if (!elapsed || *elapsed != 7
                    || !sameRegisters(cpu.registers(), expected)) {
                    std::cerr << "LD immediate destination=" << destination
                              << " value=" << value << " PC=" << pc << '\n';
                    return false;
                }
            }
        }
    }
    return true;
}

bool missingImmediatePreservesState() {
    for (const auto opcode : immediateOpcodes) {
        auto initial = initialRegisters();
        initial.pc = 0xffff;
        initial.r = 0xff;
        Z80 cpu(initial);
        if (cpu.executeOpcode(opcode).has_value()
            || !sameRegisters(cpu.registers(), initial)) {
            return false;
        }
    }
    return true;
}

bool incDecRegisters(bool decrement) {
    const auto& opcodes = decrement ? decOpcodes : incOpcodes;
    const std::array<uint16_t Z80Registers::*, 7> pairs{
        &Z80Registers::bc, &Z80Registers::bc,
        &Z80Registers::de, &Z80Registers::de,
        &Z80Registers::hl, &Z80Registers::hl, &Z80Registers::af};
    for (size_t target = 0; target < pairs.size(); ++target) {
        for (unsigned input = 0; input < 256; ++input) {
            for (const uint16_t flags : {0x00, 0x01, 0xfe, 0xff}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + flags);
                auto& pair = expected.*pairs[target];
                pair = static_cast<uint16_t>(target % 2 == 0
                    ? input * 256 + pair % 256 : pair / 256 * 256 + input);
                expected.pc = input % 2 == 0 ? 0x1234 : 0xffff;
                expected.r = static_cast<uint8_t>(input);
                Z80 cpu(expected);

                const unsigned result = (input + (decrement ? 255 : 1)) % 256;
                pair = static_cast<uint16_t>(target % 2 == 0
                    ? result * 256 + pair % 256 : pair / 256 * 256 + result);
                // Mathematical expectations, independent of opcode/flag bit tricks.
                const int signedInput = static_cast<int>(input) - (input < 128 ? 0 : 256);
                const int signedResult = signedInput + (decrement ? -1 : 1);
                const bool halfCarry = decrement
                    ? result % 16 > input % 16 : result % 16 < input % 16;
                const unsigned expectedFlags = (flags % 2)
                    + (result >= 128 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
                    + (result / 32 % 2) * 0x20 + (halfCarry ? 0x10 : 0)
                    + (result / 8 % 2) * 0x08
                    + (signedResult < -128 || signedResult > 127 ? 0x04 : 0)
                    + (decrement ? 0x02 : 0);
                expected.af = static_cast<uint16_t>(expected.af / 256 * 256 + expectedFlags);
                expected.pc = input % 2 == 0 ? 0x1235 : 0;
                expected.r = static_cast<uint8_t>(
                    input == 0x7f ? 0 : input == 0xff ? 0x80 : input + 1);

                const auto elapsed = cpu.executeOpcode(opcodes[target]);
                if (!elapsed || *elapsed != 4 || !sameRegisters(cpu.registers(), expected)) {
                    std::cerr << (decrement ? "DEC" : "INC") << " target=" << target
                              << " input=" << input << " initial F=" << flags << '\n';
                    return false;
                }
            }
        }
    }
    return true;
}

bool incDecFlagBoundaries() {
    struct Vector { uint8_t opcode, input, result, flags; };
    const std::array<Vector, 14> vectors{{
        {0x3c, 0x00, 0x01, 0x00}, {0x3c, 0x0f, 0x10, 0x10},
        {0x3c, 0x7f, 0x80, 0x94}, {0x3c, 0xff, 0x00, 0x50},
        {0x3c, 0x27, 0x28, 0x28}, {0x3c, 0xa7, 0xa8, 0xa8},
        {0x3c, 0xfe, 0xff, 0xa8}, {0x3d, 0x00, 0xff, 0xba},
        {0x3d, 0x01, 0x00, 0x42}, {0x3d, 0x10, 0x0f, 0x1a},
        {0x3d, 0x80, 0x7f, 0x3e}, {0x3d, 0x28, 0x27, 0x22},
        {0x3d, 0x29, 0x28, 0x2a}, {0x3d, 0xff, 0xfe, 0xaa},
    }};
    for (const auto& vector : vectors) {
        for (const uint8_t carry : {0, 1}) {
            auto initial = initialRegisters();
            initial.af = static_cast<uint16_t>(vector.input * 256 + 0xfe + carry);
            Z80 cpu(initial);
            const auto elapsed = cpu.executeOpcode(vector.opcode);
            if (!elapsed || *elapsed != 4 || cpu.registers().a() != vector.result
                || cpu.registers().f() != vector.flags + carry) {
                std::cerr << "Flag vector opcode=" << unsigned(vector.opcode)
                          << " input=" << unsigned(vector.input) << '\n';
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        bool passed = false;
        if (name == "registers") {
            passed = registersAreOwned();
        } else if (name == "bytes") {
            passed = registerBytesMatchPairs();
        } else if (name == "ld") {
            passed = ldCopiesRegisterBytes();
        } else if (name == "immediate") {
            passed = ldImmediateWritesRegister();
        } else if (name == "missing") {
            passed = missingImmediatePreservesState();
        } else if (name == "inc") {
            passed = incDecRegisters(false);
        } else if (name == "dec") {
            passed = incDecRegisters(true);
        } else if (name == "flags") {
            passed = incDecFlagBoundaries();
        } else if (name == "nop") {
            passed = nopPreservesRegisters();
        } else if (name == "boundaries") {
            passed = nopWrapsPcAndRefresh();
        } else if (name == "unsupported") {
            passed = unsupportedOpcodesPreserveState();
        } else {
            std::cerr << "Unknown test case: " << name << '\n';
            return 1;
        }
        if (passed) {
            return 0;
        }
        std::cerr << "Failed Z80 test: " << name << '\n';
        return 1;
    }
    std::cerr << "Unknown test case\n";
    return 1;
}
