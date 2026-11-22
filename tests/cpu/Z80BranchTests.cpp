#include "Z80TestSupport.h"

#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

bool absoluteJumps() {
    for (unsigned target = 0; target < 65536; ++target) {
        for (const bool indirect : {false, true}) {
            auto expected = initialRegisters();
            expected.pc = 0xffff;
            expected.r = 0x7f;
            if (indirect) expected.hl = static_cast<uint16_t>(target);
            Z80 cpu(expected);
            expected.pc = static_cast<uint16_t>(target);
            expected.r = 0;
            const auto elapsed = indirect ? cpu.executeOpcode(0xe9)
                : cpu.executeOpcode(0xc3, static_cast<uint8_t>(target), static_cast<uint8_t>(target / 256));
            if (!elapsed || *elapsed != (indirect ? 4 : 10)
                || !sameRegisters(cpu.registers(), expected)) return false;
        }
    }
    Z80 cpu(initialRegisters());
    return !cpu.executeOpcode(0xc3) && !cpu.executeOpcode(0xc3, 0x34)
        && !cpu.executeOpcode(0xc3, std::nullopt, 0x12)
        && sameRegisters(cpu.registers(), initialRegisters());
}

bool conditionalJumps() {
    const std::array<uint8_t, 8> opcodes{0xc2, 0xca, 0xd2, 0xda, 0xe2, 0xea, 0xf2, 0xfa};
    for (unsigned condition = 0; condition < opcodes.size(); ++condition) {
        for (unsigned flags = 0; flags < 256; ++flags) {
            const std::array<bool, 8> taken{flags / 64 % 2 == 0, flags / 64 % 2 != 0,
                flags % 2 == 0, flags % 2 != 0, flags / 4 % 2 == 0, flags / 4 % 2 != 0,
                flags / 128 == 0, flags / 128 != 0};
            for (const unsigned target : {0u, 1u, 0x1234u, 0x8000u, 0xfffeu, 0xffffu}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + flags);
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(flags);
                Z80 cpu(expected);
                expected.pc = static_cast<uint16_t>(taken[condition] ? target : 2);
                expected.r = static_cast<uint8_t>(flags == 127 ? 0 : flags == 255 ? 128 : flags + 1);
                const auto elapsed = cpu.executeOpcode(opcodes[condition], static_cast<uint8_t>(target),
                    static_cast<uint8_t>(target / 256));
                if (!elapsed || *elapsed != 10 || !sameRegisters(cpu.registers(), expected)) return false;
                Z80 missing(cpu.registers());
                if (missing.executeOpcode(opcodes[condition], 0x34)
                    || !sameRegisters(missing.registers(), cpu.registers())) return false;
            }
        }
    }
    return true;
}

bool relativeJumps() {
    const std::array<uint8_t, 5> opcodes{0x18, 0x20, 0x28, 0x30, 0x38};
    for (unsigned condition = 0; condition < opcodes.size(); ++condition) {
        for (unsigned flags = 0; flags < 256; ++flags) {
            const std::array<bool, 5> taken{true, flags / 64 % 2 == 0, flags / 64 % 2 != 0,
                flags % 2 == 0, flags % 2 != 0};
            for (int displacement = -128; displacement <= 127; ++displacement) {
                for (const unsigned pc : {0u, 0x7fffu, 0xffffu}) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.pc = static_cast<uint16_t>(pc);
                    expected.r = 0xff;
                    Z80 cpu(expected);
                    expected.pc = static_cast<uint16_t>(
                        (static_cast<int>(pc) + 2 + (taken[condition] ? displacement : 0) + 65536) % 65536);
                    expected.r = 0x80;
                    const auto elapsed = cpu.executeOpcode(opcodes[condition], static_cast<uint8_t>(displacement));
                    if (!elapsed || *elapsed != (taken[condition] ? 12 : 7)
                        || !sameRegisters(cpu.registers(), expected)) return false;
                }
            }
            auto initial = initialRegisters();
            initial.af = static_cast<uint16_t>(0x1200 + flags);
            Z80 missing(initial);
            if (missing.executeOpcode(opcodes[condition]) || !sameRegisters(missing.registers(), initial)) return false;
        }
    }
    return true;
}

bool decrementAndJump() {
    for (unsigned b = 0; b < 256; ++b) {
        for (int displacement = -128; displacement <= 127; ++displacement) {
            for (const unsigned pc : {0u, 0x7fffu, 0xffffu}) {
                auto expected = initialRegisters();
                expected.bc = static_cast<uint16_t>(b * 256 + 0x45);
                expected.af = static_cast<uint16_t>(0x1200 + b);
                expected.pc = static_cast<uint16_t>(pc);
                expected.r = 0x7f;
                Z80 cpu(expected);
                const unsigned resultB = (b + 255) % 256;
                expected.bc = static_cast<uint16_t>(resultB * 256 + 0x45);
                expected.pc = static_cast<uint16_t>((static_cast<int>(pc) + 2
                    + (resultB != 0 ? displacement : 0) + 65536) % 65536);
                expected.r = 0;
                const auto elapsed = cpu.executeOpcode(0x10, static_cast<uint8_t>(displacement));
                if (!elapsed || *elapsed != (resultB != 0 ? 13 : 8)
                    || !sameRegisters(cpu.registers(), expected)) return false;
            }
        }
    }
    Z80 missing(initialRegisters());
    return !missing.executeOpcode(0x10) && sameRegisters(missing.registers(), initialRegisters());
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "jp") return absoluteJumps() ? 0 : 1;
        if (name == "conditional") return conditionalJumps() ? 0 : 1;
        if (name == "relative") return relativeJumps() ? 0 : 1;
        if (name == "djnz") return decrementAndJump() ? 0 : 1;
    }
    std::cerr << "Unknown branch test\n";
    return 1;
}
