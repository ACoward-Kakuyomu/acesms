#include "Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::Z80Registers;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::Memory;
using smsgg::test::run;

namespace {

bool transfers(bool push) {
    struct Form { std::vector<uint8_t> bytes; uint16_t Z80Registers::*pair; };
    const uint8_t base = push ? 0xc5 : 0xc1;
    const std::array<Form, 9> forms{{
        {{base}, &Z80Registers::bc}, {{static_cast<uint8_t>(base + 0x10)}, &Z80Registers::de},
        {{static_cast<uint8_t>(base + 0x20)}, &Z80Registers::hl},
        {{static_cast<uint8_t>(base + 0x30)}, &Z80Registers::af},
        {{0xdd, static_cast<uint8_t>(base + 0x20)}, &Z80Registers::ix},
        {{0xfd, static_cast<uint8_t>(base + 0x20)}, &Z80Registers::iy},
        {{0xdd, static_cast<uint8_t>(base + 0x30)}, &Z80Registers::af},
        {{0xfd, base}, &Z80Registers::bc},
        {{0xdd, 0xfd, static_cast<uint8_t>(base + 0x20)}, &Z80Registers::iy},
    }};
    Memory memory;
    for (const auto& form : forms) {
        for (unsigned value = 0; value < 65536; ++value) {
            for (const uint16_t sp : {0, 1, 0x0802, 0xffff}) {
                auto expected = initialRegisters();
                expected.*form.pair = static_cast<uint16_t>(push ? value : 65535 - value);
                expected.sp = sp;
                expected.r = static_cast<uint8_t>(value);
                Z80 cpu(expected);
                if (!run(cpu, {0xc3, 0xff, 0xff}, &memory)) return false; // MEMPTR seed, PC wrap.
                expected = cpu.registers();
                const auto first = static_cast<uint16_t>(push ? sp - 1 : sp);
                const auto second = static_cast<uint16_t>(push ? sp - 2 : sp + 1);
                const auto low = static_cast<uint8_t>(value);
                const auto high = static_cast<uint8_t>(value / 256);
                memory.bytes[first] = push ? static_cast<uint8_t>(255 - high) : low;
                memory.bytes[second] = push ? static_cast<uint8_t>(255 - low) : high;
                memory.accesses.clear();
                const auto elapsed = cpu.executeInstruction(form.bytes, &memory);
                if (!push) expected.*form.pair = static_cast<uint16_t>(value);
                expected.sp = static_cast<uint16_t>(sp + (push ? -2 : 2));
                expected.pc = static_cast<uint16_t>(expected.pc + form.bytes.size());
                const unsigned r = expected.r;
                expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + form.bytes.size()) % 128);
                if (!elapsed || *elapsed != (push ? 11 : 10) + (form.bytes.size() - 1) * 4
                    || !sameRegisters(cpu.registers(), expected) || memory.accesses.size() != 2
                    || memory.accesses[0].write != push || memory.accesses[0].address != first
                    || memory.accesses[0].value != (push ? high : low) || memory.accesses[1].write != push
                    || memory.accesses[1].address != second || memory.accesses[1].value != (push ? low : high)
                    || memory.bytes[first] != (push ? high : low) || memory.bytes[second] != (push ? low : high)) return false;
                const unsigned xy = expected.a() | expected.f();
                if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)) return false;
                memory.bytes[cpu.registers().hl] = 0;
                if (!run(cpu, {0xcb, 0x46}, &memory) || (cpu.registers().f() & 0x28) != 0x28) return false;
            }
        }
    }
    return true;
}

bool exchange() {
    struct Form { std::vector<uint8_t> bytes; uint16_t Z80Registers::*pair; };
    const std::array<Form, 4> forms{{
        {{0xe3}, &Z80Registers::hl}, {{0xdd, 0xe3}, &Z80Registers::ix},
        {{0xfd, 0xe3}, &Z80Registers::iy}, {{0xdd, 0xfd, 0xe3}, &Z80Registers::iy},
    }};
    Memory memory;
    for (const auto& form : forms) {
        for (unsigned word = 0; word < 65536; ++word) {
            for (const uint16_t sp : {0, 0xffff}) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(0x1200 + word % 256);
                expected.*form.pair = static_cast<uint16_t>(65535 - word);
                expected.sp = sp;
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(word);
                Z80 cpu(expected);
                const auto next = static_cast<uint16_t>(sp + 1);
                const auto low = static_cast<uint8_t>(word);
                const auto high = static_cast<uint8_t>(word / 256);
                memory.bytes[sp] = low;
                memory.bytes[next] = high;
                memory.accesses.clear();
                const auto elapsed = cpu.executeInstruction(form.bytes, &memory);
                expected.*form.pair = static_cast<uint16_t>(word);
                expected.pc = static_cast<uint16_t>(expected.pc + form.bytes.size());
                const unsigned r = expected.r;
                expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + form.bytes.size()) % 128);
                if (!elapsed || *elapsed != 19 + (form.bytes.size() - 1) * 4 || !sameRegisters(cpu.registers(), expected)
                    || memory.accesses.size() != 4 || memory.accesses[0].write || memory.accesses[0].address != sp
                    || memory.accesses[0].value != low || memory.accesses[1].write || memory.accesses[1].address != next
                    || memory.accesses[1].value != high || !memory.accesses[2].write || memory.accesses[2].address != next
                    || memory.accesses[2].value != 255 - high || !memory.accesses[3].write || memory.accesses[3].address != sp
                    || memory.accesses[3].value != 255 - low) return false;
                memory.bytes[cpu.registers().hl] = 0;
                if (!run(cpu, {0xcb, 0x46}, &memory) || (cpu.registers().f() & 0x28) != ((word / 256) & 0x28)) return false;
            }
        }
        Z80 cpu(initialRegisters());
        const auto initial = cpu.registers();
        if (cpu.executeInstruction(form.bytes) || !sameRegisters(cpu.registers(), initial)) return false;
    }
    return true;
}

bool condition(unsigned flags, unsigned code) {
    const bool z = flags / 64 % 2 != 0, c = flags % 2 != 0;
    const bool pv = flags / 4 % 2 != 0, s = flags >= 128;
    const std::array<bool, 8> results{!z, z, !c, c, !pv, pv, !s, s};
    return results[code];
}

const std::vector<std::vector<uint8_t>> flowHeads{{}, {0xdd}, {0xfd, 0xdd}};

void advance(Z80Registers& expected, size_t length, size_t fetches) {
    expected.pc = static_cast<uint16_t>(expected.pc + length);
    const unsigned r = expected.r;
    expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + fetches) % 128);
}

bool memptrIs(Z80& cpu, Memory& memory, uint16_t value) {
    memory.bytes[cpu.registers().hl] = 0;
    const uint8_t f = static_cast<uint8_t>((cpu.registers().f() & 1) | 0x54 | ((value / 256) & 0x28));
    return run(cpu, {0xcb, 0x46}, &memory) && cpu.registers().f() == f;
}

bool calls(bool conditional) {
    Memory memory;
    for (unsigned code = 0; code < (conditional ? 8u : 1u); ++code) {
        for (unsigned value = 0; value < (conditional ? 256u : 65536u); ++value) {
            const unsigned flags = value % 256;
            const auto target = static_cast<uint16_t>(conditional ? value * 256 + 255 - value : value);
            const bool taken = !conditional || condition(flags, code);
            for (const uint16_t sp : {0, 1, 0xffff}) {
                for (const auto& head : flowHeads) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.pc = 0xffff;
                    expected.sp = sp;
                    expected.r = static_cast<uint8_t>(value);
                    Z80 cpu(expected);
                    auto bytes = head;
                    bytes.insert(bytes.end(), {static_cast<uint8_t>(conditional ? 0xc4 + code * 8 : 0xcd),
                        static_cast<uint8_t>(target), static_cast<uint8_t>(target / 256)});
                    advance(expected, bytes.size(), head.size() + 1);
                    const uint16_t returnAddress = expected.pc;
                    if (taken) { expected.pc = target; expected.sp = static_cast<uint16_t>(sp - 2); }
                    memory.accesses.clear();
                    const auto elapsed = cpu.executeInstruction(bytes, &memory);
                    if (!elapsed || *elapsed != (taken ? 17 : 10) + head.size() * 4
                        || !sameRegisters(cpu.registers(), expected) || memory.accesses.size() != (taken ? 2 : 0)) return false;
                    if (taken && (!memory.accesses[0].write || memory.accesses[0].address != static_cast<uint16_t>(sp - 1)
                        || memory.accesses[0].value != returnAddress / 256 || !memory.accesses[1].write
                        || memory.accesses[1].address != static_cast<uint16_t>(sp - 2)
                        || memory.accesses[1].value != returnAddress % 256)) return false;
                    if (!memptrIs(cpu, memory, target)) return false;
                }
            }
        }
    }
    return true;
}

bool returns(bool conditional) {
    Memory memory;
    for (unsigned code = 0; code < (conditional ? 8u : 1u); ++code) {
        for (unsigned value = 0; value < (conditional ? 256u : 65536u); ++value) {
            const unsigned flags = value % 256;
            const auto target = static_cast<uint16_t>(conditional ? value * 256 + 255 - value : value);
            const bool taken = !conditional || condition(flags, code);
            for (const uint16_t sp : {0, 1, 0xffff}) {
                for (const auto& head : flowHeads) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.sp = sp;
                    expected.r = static_cast<uint8_t>(value);
                    Z80 cpu(expected);
                    if (!run(cpu, {0xc3, 0, 0x28}, &memory)) return false;
                    expected = cpu.registers();
                    memory.bytes[sp] = static_cast<uint8_t>(target);
                    memory.bytes[static_cast<uint16_t>(sp + 1)] = static_cast<uint8_t>(target / 256);
                    auto bytes = head;
                    bytes.push_back(static_cast<uint8_t>(conditional ? 0xc0 + code * 8 : 0xc9));
                    advance(expected, bytes.size(), head.size() + 1);
                    if (taken) { expected.pc = target; expected.sp = static_cast<uint16_t>(sp + 2); }
                    memory.accesses.clear();
                    const auto elapsed = cpu.executeInstruction(bytes, &memory);
                    if (!elapsed || *elapsed != (taken ? (conditional ? 11 : 10) : 5) + head.size() * 4
                        || !sameRegisters(cpu.registers(), expected) || memory.accesses.size() != (taken ? 2 : 0)) return false;
                    if (taken && (memory.accesses[0].write || memory.accesses[0].address != sp
                        || memory.accesses[0].value != target % 256 || memory.accesses[1].write
                        || memory.accesses[1].address != static_cast<uint16_t>(sp + 1)
                        || memory.accesses[1].value != target / 256)) return false;
                    if (!memptrIs(cpu, memory, taken ? target : 0x2800)) return false;
                }
            }
        }
    }
    return true;
}

bool restart() {
    Memory memory;
    for (unsigned vector = 0; vector < 8; ++vector) {
        for (const uint16_t pc : {0, 0xfffe, 0xffff}) {
            for (const uint16_t sp : {0, 1, 0xffff}) {
                for (const auto& head : flowHeads) {
                    for (unsigned r = 0; r < 256; ++r) {
                        auto expected = initialRegisters();
                        expected.pc = pc; expected.sp = sp; expected.r = static_cast<uint8_t>(r);
                        Z80 cpu(expected);
                        auto bytes = head;
                        bytes.push_back(static_cast<uint8_t>(0xc7 + vector * 8));
                        advance(expected, bytes.size(), head.size() + 1);
                        const uint16_t returnAddress = expected.pc;
                        expected.pc = static_cast<uint16_t>(vector * 8);
                        expected.sp = static_cast<uint16_t>(sp - 2);
                        memory.accesses.clear();
                        const auto elapsed = cpu.executeInstruction(bytes, &memory);
                        if (!elapsed || *elapsed != 11 + head.size() * 4 || !sameRegisters(cpu.registers(), expected)
                            || memory.accesses.size() != 2 || !memory.accesses[0].write
                            || memory.accesses[0].address != static_cast<uint16_t>(sp - 1)
                            || memory.accesses[0].value != returnAddress / 256 || !memory.accesses[1].write
                            || memory.accesses[1].address != static_cast<uint16_t>(sp - 2)
                            || memory.accesses[1].value != returnAddress % 256) return false;
                    }
                }
            }
        }
        Z80 cpu(initialRegisters());
        if (!run(cpu, {0xc3, 0, 0x28}, &memory)
            || !run(cpu, {static_cast<uint8_t>(0xc7 + vector * 8)}, &memory) || !memptrIs(cpu, memory, 0)) return false;
    }
    return true;
}

bool flowMissing() {
    Memory memory;
    for (unsigned code = 0; code < 8; ++code) {
        for (unsigned flags = 0; flags < 256; ++flags) {
            auto initial = initialRegisters();
            initial.af = static_cast<uint16_t>(0x1200 + flags);
            for (const bool call : {false, true}) {
                Z80 cpu(initial);
                std::vector<uint8_t> bytes{0xdd, static_cast<uint8_t>((call ? 0xc4 : 0xc0) + code * 8)};
                if (call) bytes.insert(bytes.end(), {0x34, 0x12});
                const bool taken = condition(flags, code);
                const auto elapsed = cpu.executeInstruction(bytes);
                if (taken ? elapsed.has_value() || !sameRegisters(cpu.registers(), initial)
                    : !elapsed || *elapsed != (call ? 14 : 9) || cpu.registers().sp != initial.sp) return false;
            }
            Z80 cpu(initial);
            memory.accesses.clear();
            if (run(cpu, {static_cast<uint8_t>(0xc4 + code * 8)}, &memory)
                || run(cpu, {static_cast<uint8_t>(0xc4 + code * 8), 0x34}, &memory)
                || !sameRegisters(cpu.registers(), initial) || !memory.accesses.empty()) return false;
        }
    }
    for (const uint8_t opcode : {0xcd, 0xc9, 0xc7, 0xcf, 0xd7, 0xdf, 0xe7, 0xef, 0xf7, 0xff}) {
        Z80 cpu(initialRegisters());
        const auto before = cpu.registers();
        if (run(cpu, {opcode, 0, 0}, nullptr) || !sameRegisters(cpu.registers(), before)) return false;
    }
    for (const uint8_t opcode : {0xc0, 0xc4}) {
        auto initial = initialRegisters(); initial.af = 0; initial.bc = 0x2828;
        Z80 cpu(initial);
        if (!run(cpu, {0xcb, 0x40}, &memory) || !run(cpu, {opcode, 0, 0}, nullptr)
            || !run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != 0x28) return false;
    }
    Z80 enabled(initialRegisters());
    return run(enabled, {0xfb}, &memory) && !run(enabled, {0xcd, 0}, &memory)
        && !enabled.canAcceptMaskableInterrupt() && run(enabled, {0xdd, 0xc4, 0, 0}, nullptr)
        && enabled.canAcceptMaskableInterrupt();
}

bool interruptReturns() {
    Memory memory;
    for (const uint8_t opcode : {0x45, 0x4d, 0x55, 0x5d, 0x65, 0x6d, 0x75, 0x7d}) {
        for (unsigned value = 0; value < 256; ++value) {
            for (const bool enabled : {false, true}) {
                for (const uint16_t sp : {0, 1, 0xffff}) {
                    for (const auto& head : flowHeads) {
                        auto expected = initialRegisters();
                        expected.af = static_cast<uint16_t>(0x1200 + value);
                        expected.sp = sp;
                        expected.r = static_cast<uint8_t>(value);
                        Z80 cpu(expected);
                        if (enabled && !run(cpu, {0xfb}, &memory)) return false;
                        expected = cpu.registers();
                        const auto target = static_cast<uint16_t>(value * 256 + 255 - value);
                        memory.bytes[sp] = static_cast<uint8_t>(255 - value);
                        memory.bytes[static_cast<uint16_t>(sp + 1)] = static_cast<uint8_t>(value);
                        memory.accesses.clear();
                        auto bytes = head; bytes.insert(bytes.end(), {0xed, opcode});
                        const auto elapsed = cpu.executeInstruction(bytes, &memory);
                        advance(expected, bytes.size(), head.size() + 2);
                        expected.pc = target;
                        expected.sp = static_cast<uint16_t>(sp + 2);
                        if (!elapsed || *elapsed != 14 + head.size() * 4 || !sameRegisters(cpu.registers(), expected)
                            || cpu.iff1() != enabled || cpu.iff2() != enabled || cpu.canAcceptMaskableInterrupt() != enabled
                            || memory.accesses.size() != 2 || memory.accesses[0].write || memory.accesses[0].address != sp
                            || memory.accesses[0].value != 255 - value || memory.accesses[1].write
                            || memory.accesses[1].address != static_cast<uint16_t>(sp + 1)
                            || memory.accesses[1].value != value || !memptrIs(cpu, memory, target)) return false;
                    }
                }
            }
        }
        Z80 cpu(initialRegisters());
        if (!run(cpu, {0xfb}, &memory)) return false;
        const auto before = cpu.registers();
        if (run(cpu, {0xed, opcode}, nullptr) || !sameRegisters(cpu.registers(), before)
            || !cpu.iff1() || !cpu.iff2() || cpu.canAcceptMaskableInterrupt()) return false;
    }
    return true;
}

bool missing() {
    Memory memory;
    for (const uint8_t opcode : {0xc5, 0xd5, 0xe5, 0xf5, 0xc1, 0xd1, 0xe1, 0xf1}) {
        Z80 cpu(initialRegisters());
        const auto initial = cpu.registers();
        if (run(cpu, {opcode}, nullptr) || run(cpu, {0xdd, opcode}, nullptr)
            || !sameRegisters(cpu.registers(), initial)) return false;
    }
    auto initial = initialRegisters();
    initial.af = 0;
    initial.bc = 0x2828;
    Z80 cpu(initial);
    if (!run(cpu, {0xcb, 0x40}, &memory) || !run(cpu, {0xc5}, &memory)
        || !run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != 0x28) return false;
    Z80 enabled(initialRegisters());
    return run(enabled, {0xfb}, &memory) && !run(enabled, {0xc5}, nullptr)
        && !enabled.canAcceptMaskableInterrupt() && run(enabled, {0xdd, 0xe5}, &memory)
        && enabled.canAcceptMaskableInterrupt();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "exchange") return exchange() ? 0 : 1;
        if (name == "call") return calls(false) ? 0 : 1;
        if (name == "call_conditional") return calls(true) ? 0 : 1;
        if (name == "ret") return returns(false) ? 0 : 1;
        if (name == "ret_conditional") return returns(true) ? 0 : 1;
        if (name == "rst") return restart() ? 0 : 1;
        if (name == "flow_missing") return flowMissing() ? 0 : 1;
        if (name == "interrupt_return") return interruptReturns() ? 0 : 1;
        const bool passed = name == "push" ? transfers(true) : name == "pop" ? transfers(false)
            : name == "missing" && missing();
        if (passed) return 0;
    }
    std::cerr << "Failed Z80 stack test\n";
    return 1;
}
