#include "Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::Memory;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::run;

namespace {

bool pushed(const Memory& memory, uint16_t sp, uint16_t pc, size_t accesses = 2) {
    return memory.accesses.size() == accesses && memory.accesses[0].write
        && memory.accesses[0].address == static_cast<uint16_t>(sp - 1)
        && memory.accesses[0].value == pc / 256 && memory.accesses[1].write
        && memory.accesses[1].address == static_cast<uint16_t>(sp - 2)
        && memory.accesses[1].value == pc % 256;
}

bool memptrIs(Z80& cpu, Memory& memory, uint16_t value) {
    memory.bytes[cpu.registers().hl] = 0;
    const auto flags = static_cast<uint8_t>((cpu.registers().f() & 1) | 0x54 | ((value / 256) & 0x28));
    return run(cpu, {0xcb, 0x46}, &memory) && cpu.registers().f() == flags;
}

bool nmi() {
    Memory memory;
    for (unsigned pc = 0; pc < 65536; ++pc) {
        for (const uint16_t sp : {0, 1, 0xffff}) {
            for (const bool enabled : {false, true}) {
                auto expected = initialRegisters();
                expected.pc = static_cast<uint16_t>(pc); expected.sp = sp;
                expected.r = static_cast<uint8_t>(pc); expected.setF(static_cast<uint8_t>(pc >> 8));
                Z80 cpu(expected);
                const uint8_t mode = static_cast<uint8_t>(pc % 3);
                if (!run(cpu, {0xed, static_cast<uint8_t>(mode == 0 ? 0x46 : mode == 1 ? 0x56 : 0x5e)}, &memory)) return false;
                // EI's pending IRQ delay must not prevent NMI.
                if (enabled && !run(cpu, {0xfb}, &memory)) return false;
                if (pc % 2 != 0 && !run(cpu, {0x76}, &memory)) return false;
                expected = cpu.registers();
                const uint16_t savedPc = expected.pc;
                memory.accesses.clear();
                const auto time = cpu.acceptNmi(memory);
                expected.pc = 0x66; expected.sp = static_cast<uint16_t>(sp - 2);
                const unsigned r = expected.r;
                expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + 1) % 128);
                if (time != 11 || !sameRegisters(cpu.registers(), expected) || !pushed(memory, sp, savedPc)
                    || cpu.halted() || cpu.iff1() || cpu.iff2() != enabled
                    || cpu.canAcceptMaskableInterrupt() || cpu.interruptMode() != mode) return false;
                if (!memptrIs(cpu, memory, 0x66)) return false;
            }
        }
    }
    // A flag-writing instruction followed by NMI loses its Q history.
    auto initial = initialRegisters(); initial.af = 0x0000; initial.bc = 0x2700;
    Z80 cpu(initial);
    return run(cpu, {0x04}, &memory) && cpu.acceptNmi(memory) == 11
        && run(cpu, {0x37}, &memory) && (cpu.registers().f() & 0x28) == 0x28;
}

bool nmiReturn() {
    Memory memory;
    for (const uint8_t opcode : {0x45, 0x4d, 0x55, 0x5d, 0x65, 0x6d, 0x75, 0x7d}) {
        for (const bool enabled : {false, true}) {
            for (const bool nested : {false, true}) {
                Z80 cpu(initialRegisters());
                if (enabled && !run(cpu, {0xfb}, &memory)) return false;
                const uint16_t savedPc = cpu.registers().pc, savedSp = cpu.registers().sp;
                if (cpu.acceptNmi(memory) != 11 || cpu.iff1() || cpu.iff2() != enabled) return false;
                if (nested) {
                    if (cpu.acceptNmi(memory) != 11 || cpu.iff2() != enabled
                        || !run(cpu, {0xed, opcode}, &memory) || cpu.registers().pc != 0x66) return false;
                }
                if (!run(cpu, {0xed, opcode}, &memory) || cpu.registers().pc != savedPc
                    || cpu.registers().sp != savedSp || cpu.iff1() != enabled || cpu.iff2() != enabled
                    || cpu.canAcceptMaskableInterrupt() != enabled) return false;
            }
        }
        for (const bool enableInHandler : {false, true}) {
            Z80 cpu(initialRegisters());
            if (!run(cpu, {0xfb}, &memory) || cpu.acceptNmi(memory) != 11
                || !run(cpu, {static_cast<uint8_t>(enableInHandler ? 0xfb : 0xf3)}, &memory)
                || !run(cpu, {0xed, opcode}, &memory)
                || cpu.iff1() != enableInHandler || cpu.iff2() != enableInHandler) return false;
        }
    }
    Z80 cpu(initialRegisters());
    if (!run(cpu, {0xfb}, &memory) || cpu.acceptNmi(memory) != 11) return false;
    cpu.reset();
    return !cpu.iff1() && !cpu.iff2() && !cpu.halted() && cpu.registers().pc == 0;
}

bool irq(bool mode2) {
    Memory memory;
    for (unsigned value = 0; value < 65536; ++value) {
        for (const uint16_t sp : {0, 1, 0xbcde}) {
            auto expected = initialRegisters();
            expected.pc = static_cast<uint16_t>(value); expected.sp = sp;
            expected.setF(static_cast<uint8_t>(value >> 8)); expected.r = static_cast<uint8_t>(value);
            expected.i = static_cast<uint8_t>(value >> 8);
            Z80 cpu(expected);
            if (!run(cpu, {0xed, static_cast<uint8_t>(mode2 ? 0x5e : 0x56)}, &memory)
                || !run(cpu, {0xfb}, &memory)
                || !run(cpu, {static_cast<uint8_t>(value % 2 == 0 ? 0 : 0x76)}, &memory)) return false;
            expected = cpu.registers();
            const uint16_t savedPc = expected.pc;
            const uint16_t address = static_cast<uint16_t>(value), next = static_cast<uint16_t>(address + 1);
            memory.bytes[address] = static_cast<uint8_t>(~value);
            memory.bytes[next] = static_cast<uint8_t>((~value) >> 8);
            uint8_t low = memory.bytes[address], high = memory.bytes[next];
            if (address == static_cast<uint16_t>(sp - 1)) low = static_cast<uint8_t>(savedPc >> 8);
            if (address == static_cast<uint16_t>(sp - 2)) low = static_cast<uint8_t>(savedPc);
            if (next == static_cast<uint16_t>(sp - 1)) high = static_cast<uint8_t>(savedPc >> 8);
            if (next == static_cast<uint16_t>(sp - 2)) high = static_cast<uint8_t>(savedPc);
            const uint16_t target = mode2 ? static_cast<uint16_t>(low + high * 256) : 0x38;
            memory.accesses.clear();
            const std::array<uint8_t, 1> vector{static_cast<uint8_t>(value)};
            const auto time = cpu.acceptInterrupt(vector, &memory);
            expected.pc = target; expected.sp = static_cast<uint16_t>(sp - 2);
            const unsigned r = expected.r;
            expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + 1) % 128);
            if (!time || *time != (mode2 ? 19 : 13) || !sameRegisters(cpu.registers(), expected)
                || cpu.iff1() || cpu.iff2() || cpu.halted() || cpu.canAcceptMaskableInterrupt()
                || !pushed(memory, sp, savedPc, mode2 ? 4 : 2)) return false;
            if (mode2 && (memory.accesses[2].write || memory.accesses[2].address != address
                || memory.accesses[2].value != low || memory.accesses[3].write
                || memory.accesses[3].address != next || memory.accesses[3].value != high)) return false;
            if (!memptrIs(cpu, memory, target)) return false;
        }
    }
    return true;
}

bool irqGate() {
    Memory memory;
    Z80 cpu(initialRegisters());
    const auto initial = cpu.registers();
    auto time = cpu.acceptInterrupt({});
    if (!time || *time != 0 || !sameRegisters(cpu.registers(), initial) || !memory.accesses.empty()) return false;
    if (!run(cpu, {0xed, 0x56}, &memory) || !run(cpu, {0xfb}, &memory)) return false;
    const auto afterEi = cpu.registers();
    time = cpu.acceptInterrupt({}, &memory);
    if (!time || *time != 0 || !sameRegisters(cpu.registers(), afterEi)) return false;
    if (!run(cpu, {0xfb}, &memory) || run(cpu, {0xdd}, &memory)) return false;
    time = cpu.acceptInterrupt({}, &memory);
    if (!time || *time != 0 || cpu.canAcceptMaskableInterrupt()) return false;
    if (!run(cpu, {0xdd, 0xfd, 0x76}, &memory) || !cpu.halted() || !cpu.canAcceptMaskableInterrupt()) return false;
    const auto halted = cpu.registers();
    if (cpu.acceptInterrupt({}) || !sameRegisters(cpu.registers(), halted) || !cpu.iff1() || !cpu.halted()) return false;
    time = cpu.acceptInterrupt({}, &memory);
    if (!time || *time != 13 || cpu.halted() || !run(cpu, {0xed, 0x4d}, &memory)
        || cpu.iff1() || cpu.iff2() || cpu.registers().pc != halted.pc) return false;
    // A handler that enables IRQ before RETI becomes eligible after RETI.
    if (!run(cpu, {0xfb}, &memory) || !run(cpu, {0}, &memory)
        || !cpu.acceptInterrupt({}, &memory) || !run(cpu, {0xfb}, &memory)
        || !run(cpu, {0xed, 0x4d}, &memory) || !cpu.canAcceptMaskableInterrupt()) return false;
    if (cpu.acceptNmi(memory) != 11) return false;
    const auto afterNmi = cpu.registers(); memory.accesses.clear();
    time = cpu.acceptInterrupt({}, &memory);
    if (!time || *time != 0 || !sameRegisters(cpu.registers(), afterNmi) || !memory.accesses.empty()) return false;
    Z80 vectorMissing(initialRegisters());
    if (!run(vectorMissing, {0xed, 0x5e}, &memory) || !run(vectorMissing, {0xfb}, &memory)
        || !run(vectorMissing, {0}, &memory)) return false;
    const auto beforeMissing = vectorMissing.registers();
    if (vectorMissing.acceptInterrupt({}, &memory) || !sameRegisters(vectorMissing.registers(), beforeMissing)
        || !vectorMissing.canAcceptMaskableInterrupt() || !memory.accesses.empty()) return false;
    const std::array<uint8_t, 1> vector{0xff};
    if (vectorMissing.acceptInterrupt(vector) || !sameRegisters(vectorMissing.registers(), beforeMissing)
        || !vectorMissing.canAcceptMaskableInterrupt()) return false;
    if (!run(vectorMissing, {0xf3}, &memory) || !run(vectorMissing, {0x76}, &memory)) return false;
    const auto disabledHalt = vectorMissing.registers();
    time = vectorMissing.acceptInterrupt(vector, &memory);
    return time && *time == 0 && vectorMissing.halted()
        && sameRegisters(vectorMissing.registers(), disabledHalt) && memory.accesses.empty();
}

class Io final : public smsgg::Z80Io {
public:
    unsigned calls{};
    uint16_t port{};
    uint8_t data{};
    uint64_t writeTstate{};
    uint8_t readPort(uint16_t address, uint64_t) override { ++calls; port = address; data = 0x11; return data; }
    void writePort(uint16_t address, uint8_t value, uint64_t tstate) override {
        ++calls; port = address; data = value; writeTstate = tstate;
    }
};

bool im0() {
    Memory memory;
    struct Vector { std::vector<uint8_t> bytes; uint16_t pc; uint32_t time; };
    const std::array<Vector, 12> vectors{{
        {{0}, 0x1236, 6}, {{0x3e, 0x42}, 0x1237, 9}, {{0xc3, 0x78, 0x56}, 0x5678, 12},
        {{0x18, 0xfe}, 0x1235, 14}, {{0xcd, 0x78, 0x56}, 0x5678, 19}, {{0xff}, 0x38, 13},
        {{0x76}, 0x1236, 6}, {{0xfb}, 0x1236, 6}, {{0xcb, 0x00}, 0x1237, 10},
        {{0xed, 0x44}, 0x1237, 10}, {{0xdd, 0x21, 0x78, 0x56}, 0x1239, 16},
        {{0xfd, 0xcb, 0xff, 0x06}, 0x1239, 25},
    }};
    for (const auto& vector : vectors) {
        Z80 cpu(initialRegisters());
        if (!run(cpu, {0xfb}, &memory) || !run(cpu, {0}, &memory)) return false;
        const auto before = cpu.registers(); memory.accesses.clear();
        const auto time = cpu.acceptInterrupt(vector.bytes, &memory);
        const unsigned fetches = vector.bytes[0] == 0xcb || vector.bytes[0] == 0xed
            || vector.bytes[0] == 0xdd || vector.bytes[0] == 0xfd ? 2 : 1;
        if (!time || *time != vector.time || cpu.registers().pc != vector.pc
            || cpu.registers().r != before.r / 128 * 128 + (before.r % 128 + fetches) % 128
            || cpu.iff1() != (vector.bytes[0] == 0xfb) || cpu.iff2() != (vector.bytes[0] == 0xfb)
            || cpu.halted() != (vector.bytes[0] == 0x76)) return false;
        if (vector.bytes[0] == 0xcd || vector.bytes[0] == 0xff) {
            const uint16_t savedPc = static_cast<uint16_t>(before.pc + (vector.bytes[0] == 0xcd ? 2 : 0));
            if (!pushed(memory, before.sp, savedPc)) return false;
        } else if (cpu.registers().sp != before.sp) return false;
        if (vector.bytes[0] == 0x3e && cpu.registers().a() != 0x42) return false;
        if (vector.bytes[0] == 0xdd && cpu.registers().ix != 0x5678) return false;
        if (vector.bytes[0] == 0xfb && (cpu.canAcceptMaskableInterrupt()
            || !run(cpu, {0}, &memory) || !cpu.canAcceptMaskableInterrupt())) return false;
    }
    for (const auto& bytes : std::vector<std::vector<uint8_t>>{{}, {0xcd}, {0xdd}, {0x3e}, {0xdd, 0xcb, 0}, {0xdb, 0x42}}) {
        Z80 cpu(initialRegisters());
        if (!run(cpu, {0xfb}, &memory) || !run(cpu, {0x76}, &memory)) return false;
        const auto before = cpu.registers(); memory.accesses.clear();
        if (cpu.acceptInterrupt(bytes, &memory) || !sameRegisters(cpu.registers(), before)
            || !cpu.halted() || !cpu.iff1() || !cpu.iff2() || !cpu.canAcceptMaskableInterrupt()
            || !memory.accesses.empty()) return false;
    }
    for (const uint8_t opcode : {0xdb, 0xd3}) {
        constexpr uint64_t start = 0x100000000ULL;
        Z80 cpu(initialRegisters()); Io io;
        if (!run(cpu, {0xfb}, &memory) || !run(cpu, {0}, &memory)) return false;
        const auto before = cpu.registers();
        const std::array<uint8_t, 2> bytes{opcode, 0xfe};
        const auto time = cpu.acceptInterrupt(bytes, nullptr, &io, start);
        if (!time || *time != 13 || io.calls != 1 || io.port != 0x12fe
            || io.data != (opcode == 0xdb ? 0x11 : 0x12) || cpu.registers().pc != before.pc + 1
            || io.writeTstate != (opcode == 0xdb ? 0 : start + 13)
            || cpu.registers().a() != (opcode == 0xdb ? 0x11 : 0x12)) return false;
    }
    for (const uint16_t pc : {0, 0xffff}) {
        for (const uint8_t opcode : {0xc7, 0xcf, 0xd7, 0xdf, 0xe7, 0xef, 0xf7, 0xff}) {
            auto initial = initialRegisters(); initial.pc = static_cast<uint16_t>(pc - 2); initial.sp = 0;
            Z80 cpu(initial);
            if (!run(cpu, {0xfb}, &memory) || !run(cpu, {0}, &memory)) return false;
            memory.accesses.clear();
            const std::array<uint8_t, 1> bytes{opcode};
            const auto time = cpu.acceptInterrupt(bytes, &memory);
            if (!time || *time != 13 || cpu.registers().pc != (opcode & 0x38)
                || !pushed(memory, 0, pc)) return false;
        }
    }
    auto initial = initialRegisters(); initial.af = 0; initial.bc = 0x2700;
    Z80 flags(initial);
    const std::array<uint8_t, 1> scf{0x37};
    if (!run(flags, {0xfb}, &memory) || !run(flags, {0x04}, &memory)
        || !flags.acceptInterrupt(scf) || flags.registers().f() != 0x29) return false;
    return true;
}

bool interruptPv() {
    Memory memory;
    const std::array<uint8_t, 1> rst{0xff};
    for (const uint8_t opcode : {0x57, 0x5f}) {
        for (unsigned value = 0; value < 256; ++value) {
            for (const uint8_t mode : {0, 1, 2}) {
                auto initial = initialRegisters(); initial.i = static_cast<uint8_t>(value); initial.r = static_cast<uint8_t>(value);
                Z80 cpu(initial);
                if (!run(cpu, {0xed, static_cast<uint8_t>(mode == 0 ? 0x46 : mode == 1 ? 0x56 : 0x5e)}, &memory)
                    || !run(cpu, {0xfb}, &memory) || !run(cpu, {0xed, opcode}, &memory)) return false;
                const uint8_t a = cpu.registers().a(), f = cpu.registers().f();
                if ((f & 4) == 0 || cpu.acceptInterrupt(rst) || cpu.registers().f() != f
                    || !cpu.canAcceptMaskableInterrupt()) return false;
                const auto time = cpu.acceptInterrupt(rst, &memory);
                if (!time || *time != (mode == 2 ? 19 : 13) || cpu.registers().a() != a
                    || cpu.registers().f() != (f & 0xfb)) return false;
            }
            for (const bool nmiInstead : {false, true}) {
                auto initial = initialRegisters(); initial.i = static_cast<uint8_t>(value); initial.r = static_cast<uint8_t>(value);
                Z80 cpu(initial);
                if (!run(cpu, {0xed, 0x56}, &memory) || !run(cpu, {0xfb}, &memory)
                    || !run(cpu, {0xed, opcode}, &memory)) return false;
                const auto f = cpu.registers().f();
                if (nmiInstead) {
                    if (cpu.acceptNmi(memory) != 11 || cpu.registers().f() != f) return false;
                } else if (!run(cpu, {0}, &memory) || !cpu.acceptInterrupt({}, &memory)
                    || cpu.registers().f() != f) return false;
            }
        }
    }
    return true;
}

bool blockResume() {
    Memory memory;
    Io io;
    for (const uint8_t opcode : {0xb0, 0xb8, 0xb1, 0xb9, 0xb2, 0xba, 0xb3, 0xbb}) {
        for (const bool nmiInstead : {false, true}) {
            auto initial = initialRegisters(); initial.bc = (opcode & 2) != 0 ? 0x0242 : 2;
            Z80 cpu(initial);
            if (!run(cpu, {0xed, 0x56}, &memory) || !run(cpu, {0xfb}, &memory)
                || !run(cpu, {0xc3, 0, 0x28}, &memory)) return false;
            memory.bytes[initial.hl] = 0x11;
            const int direction = (opcode & 8) != 0 ? -1 : 1;
            memory.bytes[static_cast<uint16_t>(initial.hl + direction)] = 0x11;
            const std::array<uint8_t, 3> first{0xdd, 0xed, opcode};
            auto time = cpu.executeInstruction(first, &memory, &io);
            if (!time || *time != 25 || cpu.registers().pc != 0x2801) return false;
            const auto paused = cpu.registers(); memory.accesses.clear();
            const auto interruptTime = nmiInstead ? std::optional<uint32_t>{cpu.acceptNmi(memory)}
                : cpu.acceptInterrupt({}, &memory);
            if (!interruptTime || *interruptTime != (nmiInstead ? 11 : 13)
                || !pushed(memory, paused.sp, paused.pc) || cpu.registers().af != paused.af) return false;
            if (!nmiInstead && !run(cpu, {0xfb}, &memory)) return false;
            if (!run(cpu, {0xed, static_cast<uint8_t>(nmiInstead ? 0x45 : 0x4d)}, &memory)
                || cpu.registers().pc != paused.pc || !cpu.canAcceptMaskableInterrupt()) return false;
            const std::array<uint8_t, 2> next{0xed, opcode};
            time = cpu.executeInstruction(next, &memory, &io);
            if (!time || *time != 16 || cpu.registers().pc != 0x2803
                || cpu.registers().hl != static_cast<uint16_t>(initial.hl + direction * 2)
                || ((opcode & 2) != 0 ? cpu.registers().b() != 0 : cpu.registers().bc != 0)) return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "nmi") return nmi() ? 0 : 1;
        if (name == "nmi_return") return nmiReturn() ? 0 : 1;
        if (name == "im1") return irq(false) ? 0 : 1;
        if (name == "im2") return irq(true) ? 0 : 1;
        if (name == "irq_gate") return irqGate() ? 0 : 1;
        if (name == "im0") return im0() ? 0 : 1;
        if (name == "interrupt_pv") return interruptPv() ? 0 : 1;
        if (name == "block_resume") return blockResume() ? 0 : 1;
    }
    std::cerr << "Failed Z80 interrupt test\n";
    return 1;
}
