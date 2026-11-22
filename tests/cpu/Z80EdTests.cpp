#include "Z80TestSupport.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;

namespace {

bool negate() {
    for (const uint8_t opcode : {0x44, 0x4c, 0x54, 0x5c, 0x64, 0x6c, 0x74, 0x7c}) {
        for (unsigned a = 0; a < 256; ++a) {
            for (unsigned flags = 0; flags < 256; ++flags) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(a * 256 + flags);
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(flags);
                Z80 cpu(expected);
                const unsigned result = (256 - a) % 256;
                const unsigned f = (result >= 128 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
                    + (result / 32 % 2) * 0x20 + (a % 16 != 0 ? 0x10 : 0)
                    + (result / 8 % 2) * 0x08 + (a == 128 ? 0x04 : 0) + 2 + (a != 0 ? 1 : 0);
                expected.af = static_cast<uint16_t>(result * 256 + f);
                expected.pc = 1;
                expected.r = static_cast<uint8_t>(flags / 128 * 128 + (flags % 128 + 2) % 128);
                const auto elapsed = cpu.executeOpcode(0xed, opcode);
                if (!elapsed || *elapsed != 8 || !sameRegisters(cpu.registers(), expected)) {
                    std::cerr << "NEG opcode=" << unsigned(opcode) << " a=" << a << " F=" << flags << '\n';
                    return false;
                }
            }
        }
    }
    return true;
}

bool arithmetic16(bool subtract) {
    const std::array<uint16_t Z80Registers::*, 4> pairs{
        &Z80Registers::bc, &Z80Registers::de, &Z80Registers::hl, &Z80Registers::sp};
    const std::array<uint8_t, 4> adc{0x4a, 0x5a, 0x6a, 0x7a};
    const std::array<uint8_t, 4> sbc{0x42, 0x52, 0x62, 0x72};
    for (size_t source = 0; source < pairs.size(); ++source) {
        for (int hl = 0; hl < 65536; ++hl) {
            for (const int operand : {0, 1, 0x0fff, 0x1000, 0x7fff, 0x8000, 0xffff, 0xaaaa, 0x5555}) {
                if (source == 2 && operand != 0) continue;
                for (const int carry : {0, 1}) {
                    auto expected = initialRegisters();
                    expected.*pairs[source] = static_cast<uint16_t>(operand);
                    expected.hl = static_cast<uint16_t>(hl);
                    expected.af = static_cast<uint16_t>(0x12fe + carry);
                    expected.pc = 0xffff;
                    expected.r = static_cast<uint8_t>(hl);
                    Z80 cpu(expected);
                    const int rhs = source == 2 ? hl : operand;
                    const int wide = subtract ? hl - rhs - carry : hl + rhs + carry;
                    const unsigned result = (wide + 65536) % 65536;
                    const int signedLhs = hl - (hl < 32768 ? 0 : 65536);
                    const int signedRhs = rhs - (rhs < 32768 ? 0 : 65536);
                    const int signedWide = subtract ? signedLhs - signedRhs - carry : signedLhs + signedRhs + carry;
                    const int low = subtract ? hl % 4096 - rhs % 4096 - carry : hl % 4096 + rhs % 4096 + carry;
                    const unsigned high = result / 256;
                    const unsigned flags = (result >= 32768 ? 0x80 : 0) + (result == 0 ? 0x40 : 0)
                        + (high / 32 % 2) * 0x20 + (low < 0 || low > 4095 ? 0x10 : 0)
                        + (high / 8 % 2) * 0x08 + (signedWide < -32768 || signedWide > 32767 ? 4 : 0)
                        + (subtract ? 2 : 0) + (wide < 0 || wide > 65535 ? 1 : 0);
                    expected.hl = static_cast<uint16_t>(result);
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.pc = 1;
                    const unsigned refresh = hl % 256;
                    expected.r = static_cast<uint8_t>(refresh / 128 * 128 + (refresh % 128 + 2) % 128);
                    const auto elapsed = cpu.executeOpcode(0xed, subtract ? sbc[source] : adc[source]);
                    if (!elapsed || *elapsed != 15 || !sameRegisters(cpu.registers(), expected)) {
                        std::cerr << "ED subtract=" << subtract << " source=" << source
                                  << " hl=" << hl << " rhs=" << rhs << " C=" << carry << '\n';
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool transfer(uint8_t opcode) {
    const bool readsSpecial = opcode == 0x57 || opcode == 0x5f;
    for (unsigned value = 0; value < 256; ++value) {
        for (unsigned flags = 0; flags < 256; ++flags) {
            for (const bool enabled : {false, true}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>((readsSpecial ? 0x12 : value) * 256 + flags);
                expected.i = static_cast<uint8_t>(readsSpecial ? value : 255 - value);
                expected.r = static_cast<uint8_t>(readsSpecial ? value : 255 - value);
                expected.pc = enabled ? 0xfffe : 0xffff;
                Z80 cpu(expected);
                if (enabled && !cpu.executeOpcode(0xfb)) return false;
                expected = cpu.registers();
                const unsigned oldR = expected.r;
                expected.r = static_cast<uint8_t>(oldR / 128 * 128 + (oldR % 128 + 2) % 128);
                unsigned newFlags = flags;
                if (readsSpecial) {
                    const unsigned loaded = opcode == 0x57 ? value : expected.r;
                    newFlags = (loaded >= 128 ? 0x80 : 0) + (loaded == 0 ? 0x40 : 0)
                        + (loaded / 32 % 2) * 0x20 + (loaded / 8 % 2) * 0x08
                        + (enabled ? 4 : 0) + flags % 2;
                    expected.af = static_cast<uint16_t>(loaded * 256 + newFlags);
                } else if (opcode == 0x47) {
                    expected.i = static_cast<uint8_t>(value);
                } else {
                    expected.r = static_cast<uint8_t>(value);
                }
                expected.pc = 1;
                const auto elapsed = cpu.executeOpcode(0xed, opcode);
                if (!elapsed || *elapsed != 9 || !sameRegisters(cpu.registers(), expected)
                    || cpu.iff1() != enabled || cpu.iff2() != enabled
                    || cpu.canAcceptMaskableInterrupt() != enabled) {
                    std::cerr << "ED transfer=" << unsigned(opcode) << " value=" << value
                              << " F=" << flags << " IFF=" << enabled << '\n';
                    return false;
                }
                // LD to I/R does not write F; LD from I/R does, including identical F values.
                const unsigned xy = expected.a() | (readsSpecial ? 0 : newFlags);
                const unsigned scfFlags = (newFlags & 0xc4) | (xy & 0x28) | 1;
                if (!cpu.executeOpcode(0x37) || cpu.registers().f() != scfFlags) return false;
            }
        }
    }
    return true;
}

bool interruptModes() {
    const std::array<uint8_t, 8> opcodes{0x46, 0x4e, 0x66, 0x6e, 0x56, 0x76, 0x5e, 0x7e};
    const std::array<uint8_t, 8> modes{0, 0, 0, 0, 1, 1, 2, 2};
    const std::array<uint8_t, 3> setup{0x46, 0x56, 0x5e};
    for (size_t oldMode = 0; oldMode < setup.size(); ++oldMode) {
        for (size_t instruction = 0; instruction < opcodes.size(); ++instruction) {
            for (unsigned flags = 0; flags < 256; ++flags) {
                for (const bool enabled : {false, true}) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.pc = enabled ? 0xfffc : 0xfffd;
                    expected.r = static_cast<uint8_t>(flags);
                    Z80 cpu(expected);
                    if (cpu.interruptMode() != 0 || !cpu.executeOpcode(0xed, setup[oldMode])
                        || cpu.interruptMode() != oldMode) return false;
                    if (enabled && !cpu.executeOpcode(0xfb)) return false;
                    expected = cpu.registers();
                    if (cpu.executeOpcode(0xed) || cpu.interruptMode() != oldMode
                        || !sameRegisters(cpu.registers(), expected)) return false;
                    expected.pc = 1;
                    const unsigned refresh = expected.r;
                    expected.r = static_cast<uint8_t>(refresh / 128 * 128 + (refresh % 128 + 2) % 128);
                    const auto elapsed = cpu.executeOpcode(0xed, opcodes[instruction]);
                    if (!elapsed || *elapsed != 8 || cpu.interruptMode() != modes[instruction]
                        || !sameRegisters(cpu.registers(), expected) || cpu.iff1() != enabled
                        || cpu.iff2() != enabled || cpu.canAcceptMaskableInterrupt() != enabled) return false;
                    const unsigned xy = expected.a() | expected.f();
                    if (!cpu.executeOpcode(0x37) || (cpu.registers().f() & 0x28) != (xy & 0x28)) return false;
                    cpu.reset();
                    if (cpu.interruptMode() != 0) return false;
                }
            }
        }
    }
    return true;
}

bool isUnused(uint8_t opcode) {
    if (opcode >= 0x40 && opcode <= 0x7f) return opcode == 0x77 || opcode == 0x7f;
    const std::array<uint8_t, 16> block{
        0xa0, 0xa1, 0xa2, 0xa3, 0xa8, 0xa9, 0xaa, 0xab,
        0xb0, 0xb1, 0xb2, 0xb3, 0xb8, 0xb9, 0xba, 0xbb};
    return std::find(block.begin(), block.end(), opcode) == block.end();
}

bool unusedOpcodes() {
    unsigned count = 0;
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        if (!isUnused(static_cast<uint8_t>(opcode))) continue;
        ++count;
        for (unsigned flags = 0; flags < 256; ++flags) {
            for (const bool enabled : {false, true}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + flags);
                expected.pc = enabled ? 0xfffc : 0xfffd;
                expected.r = static_cast<uint8_t>(flags);
                Z80 cpu(expected);
                if (!cpu.executeOpcode(0xed, 0x5e)) return false; // IM 2 must survive.
                if (enabled && !cpu.executeOpcode(0xfb)) return false;
                expected = cpu.registers();
                expected.pc = 1;
                const unsigned refresh = expected.r;
                expected.r = static_cast<uint8_t>(refresh / 128 * 128 + (refresh % 128 + 2) % 128);
                const auto elapsed = cpu.executeOpcode(0xed, static_cast<uint8_t>(opcode));
                if (!elapsed || *elapsed != 8 || !sameRegisters(cpu.registers(), expected)
                    || cpu.interruptMode() != 2 || cpu.iff1() != enabled || cpu.iff2() != enabled
                    || cpu.canAcceptMaskableInterrupt() != enabled) {
                    std::cerr << "Unused ED=" << opcode << " F=" << flags << '\n';
                    return false;
                }
                const unsigned xy = expected.a() | expected.f();
                if (!cpu.executeOpcode(0x37) || (cpu.registers().f() & 0x28) != (xy & 0x28)) return false;
            }
        }
        auto initial = initialRegisters();
        initial.bc = 0x2828;
        initial.af = 0;
        Z80 cpu(initial);
        if (!cpu.executeOpcode(0xcb, 0x40) // BIT 0,B writes F3/F5 with A still zero.
            || !cpu.executeOpcode(0xed, static_cast<uint8_t>(opcode))
            || !cpu.executeOpcode(0x37) || (cpu.registers().f() & 0x28) != 0x28) return false;
    }
    return count == 178;
}

bool missingAndUnsupported() {
    Z80 cpu(initialRegisters());
    if (cpu.executeOpcode(0xed)) return false;
    for (unsigned opcode = 0; opcode < 256; ++opcode) {
        const bool negate = opcode == 0x44 || opcode == 0x4c || opcode == 0x54 || opcode == 0x5c
            || opcode == 0x64 || opcode == 0x6c || opcode == 0x74 || opcode == 0x7c;
        const bool arithmetic = opcode == 0x42 || opcode == 0x52 || opcode == 0x62 || opcode == 0x72
            || opcode == 0x4a || opcode == 0x5a || opcode == 0x6a || opcode == 0x7a;
        const bool transfer = opcode == 0x47 || opcode == 0x4f || opcode == 0x57 || opcode == 0x5f;
        const bool mode = opcode == 0x46 || opcode == 0x4e || opcode == 0x66 || opcode == 0x6e
            || opcode == 0x56 || opcode == 0x76 || opcode == 0x5e || opcode == 0x7e;
        if (!negate && !arithmetic && !transfer && !mode && !isUnused(static_cast<uint8_t>(opcode))
            && cpu.executeOpcode(0xed, static_cast<uint8_t>(opcode))) return false;
    }
    return sameRegisters(cpu.registers(), initialRegisters());
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "neg") return negate() ? 0 : 1;
        if (name == "adc") return arithmetic16(false) ? 0 : 1;
        if (name == "sbc") return arithmetic16(true) ? 0 : 1;
        if (name == "missing") return missingAndUnsupported() ? 0 : 1;
        if (name == "ld_i_a") return transfer(0x47) ? 0 : 1;
        if (name == "ld_r_a") return transfer(0x4f) ? 0 : 1;
        if (name == "ld_a_i") return transfer(0x57) ? 0 : 1;
        if (name == "ld_a_r") return transfer(0x5f) ? 0 : 1;
        if (name == "im") return interruptModes() ? 0 : 1;
        if (name == "unused") return unusedOpcodes() ? 0 : 1;
    }
    std::cerr << "Unknown ED test\n";
    return 1;
}
