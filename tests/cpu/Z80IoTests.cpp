#include "Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::Memory;
using smsgg::test::run;

namespace {

class Io final : public smsgg::Z80Io {
public:
    struct Access { bool write; uint16_t port; uint8_t value; };
    uint8_t reply{};
    std::vector<Access> accesses;
    uint8_t readPort(uint16_t port) override {
        accesses.push_back({false, port, reply});
        return reply;
    }
    void writePort(uint16_t port, uint8_t value, uint64_t) override { accesses.push_back({true, port, value}); }
};

bool memptrIs(Z80& cpu, Memory& memory, uint16_t value) {
    memory.bytes[cpu.registers().hl] = 0;
    const auto flags = static_cast<uint8_t>((cpu.registers().f() & 1) | 0x54 | ((value / 256) & 0x28));
    return run(cpu, {0xcb, 0x46}, &memory) && cpu.registers().f() == flags;
}

bool immediate(bool write) {
    Io io;
    Memory memory;
    const std::vector<std::vector<uint8_t>> heads{{}, {0xdd}, {0xfd, 0xdd}};
    for (unsigned port = 0; port < 65536; ++port) {
        for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
            for (const auto& head : heads) {
                auto expected = initialRegisters();
                expected.af = static_cast<uint16_t>(port / 256 * 256 + flags);
                expected.pc = 0xffff;
                expected.r = static_cast<uint8_t>(port);
                Z80 cpu(expected);
                auto bytes = head;
                bytes.insert(bytes.end(), {static_cast<uint8_t>(write ? 0xd3 : 0xdb), static_cast<uint8_t>(port)});
                io.reply = static_cast<uint8_t>(255 - port / 256);
                io.accesses.clear();
                const auto elapsed = cpu.executeInstruction(bytes, nullptr, &io);
                if (!write) expected.setA(io.reply);
                expected.pc = static_cast<uint16_t>(expected.pc + bytes.size());
                const unsigned r = expected.r;
                expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + head.size() + 1) % 128);
                if (!elapsed || *elapsed != 11 + head.size() * 4 || !sameRegisters(cpu.registers(), expected)
                    || io.accesses.size() != 1 || io.accesses[0].write != write || io.accesses[0].port != port
                    || io.accesses[0].value != (write ? port / 256 : io.reply)) return false;
                const unsigned xy = expected.a() | expected.f();
                if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)) return false;
                const auto memptr = static_cast<uint16_t>(write ? port / 256 * 256 + (port + 1) % 256 : port + 1);
                if (!memptrIs(cpu, memory, memptr)) return false;
            }
        }
    }
    return true;
}

bool registers(bool write) {
    const std::array<uint8_t (smsgg::Z80Registers::*)() const, 8> get{
        &smsgg::Z80Registers::b, &smsgg::Z80Registers::c, &smsgg::Z80Registers::d, &smsgg::Z80Registers::e,
        &smsgg::Z80Registers::h, &smsgg::Z80Registers::l, nullptr, &smsgg::Z80Registers::a};
    const std::array<void (smsgg::Z80Registers::*)(uint8_t), 8> set{
        &smsgg::Z80Registers::setB, &smsgg::Z80Registers::setC, &smsgg::Z80Registers::setD, &smsgg::Z80Registers::setE,
        &smsgg::Z80Registers::setH, &smsgg::Z80Registers::setL, nullptr, &smsgg::Z80Registers::setA};
    Io io;
    Memory memory;
    const std::vector<std::vector<uint8_t>> heads{{}, {0xdd}, {0xfd, 0xdd}};
    for (unsigned code = 0; code < 8; ++code) {
        for (unsigned port = 0; port < 65536; ++port) {
            for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                for (const auto& head : heads) {
                    auto expected = initialRegisters();
                    expected.bc = static_cast<uint16_t>(port);
                    expected.af = static_cast<uint16_t>(0x1200 + flags);
                    expected.pc = 0xffff;
                    expected.r = static_cast<uint8_t>(port);
                    Z80 cpu(expected);
                    io.reply = static_cast<uint8_t>((port / 256) ^ (port % 256) ^ 0xa5);
                    const auto data = static_cast<uint8_t>(write ? (code == 6 ? 0 : (expected.*get[code])()) : io.reply);
                    io.accesses.clear();
                    auto bytes = head;
                    bytes.insert(bytes.end(), {0xed, static_cast<uint8_t>((write ? 0x41 : 0x40) + code * 8)});
                    const auto elapsed = cpu.executeInstruction(bytes, nullptr, &io);
                    if (!write) {
                        unsigned ones = 0;
                        for (unsigned bits = data; bits != 0; bits /= 2) ones += bits % 2;
                        const unsigned f = (data >= 128 ? 0x80 : 0) + (data == 0 ? 0x40 : 0)
                            + (data / 32 % 2) * 0x20 + (data / 8 % 2) * 0x08
                            + (ones % 2 == 0 ? 4 : 0) + flags % 2;
                        expected.setF(static_cast<uint8_t>(f));
                        if (code != 6) (expected.*set[code])(data);
                    }
                    expected.pc = static_cast<uint16_t>(expected.pc + bytes.size());
                    const unsigned r = expected.r;
                    expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + head.size() + 2) % 128);
                    if (!elapsed || *elapsed != 12 + head.size() * 4 || !sameRegisters(cpu.registers(), expected)
                        || io.accesses.size() != 1 || io.accesses[0].write != write
                        || io.accesses[0].port != port || io.accesses[0].value != data) return false;
                    const unsigned xy = expected.a() | (write ? expected.f() : 0);
                    if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (xy & 0x28)
                        || !memptrIs(cpu, memory, static_cast<uint16_t>(port + 1))) return false;
                }
            }
        }
        Z80 cpu(initialRegisters());
        const auto before = cpu.registers();
        if (run(cpu, {0xed, static_cast<uint8_t>((write ? 0x41 : 0x40) + code * 8)}, nullptr)
            || !sameRegisters(cpu.registers(), before)) return false;
    }
    return true;
}

bool missing() {
    Io io;
    Memory memory;
    for (const uint8_t opcode : {0xd3, 0xdb}) {
        Z80 cpu(initialRegisters());
        const auto before = cpu.registers();
        const std::array<uint8_t, 1> incomplete{opcode};
        if (cpu.executeInstruction(incomplete, nullptr, &io) || run(cpu, {opcode, 0}, nullptr)
            || !sameRegisters(cpu.registers(), before) || !io.accesses.empty()) return false;
        auto initial = initialRegisters(); initial.af = 0; initial.bc = 0x2828;
        Z80 q(initial);
        if (!run(q, {0xcb, 0x40}, &memory) || !q.executeOpcode(opcode, 0, std::nullopt, nullptr, &io)
            || !run(q, {0x37}, &memory) || (q.registers().f() & 0x28) != 0x28) return false;
        io.accesses.clear();
    }
    for (const uint8_t opcode : {0x41, 0x71}) {
        auto initial = initialRegisters(); initial.af = 0; initial.bc = 0x2828;
        Z80 cpu(initial);
        const std::array<uint8_t, 2> bytes{0xed, opcode};
        if (!run(cpu, {0xcb, 0x40}, &memory) || !cpu.executeInstruction(bytes, nullptr, &io)
            || !run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != 0x28) return false;
    }
    Z80 enabled(initialRegisters());
    const std::array<uint8_t, 2> bytes{0xdb, 0xff};
    return run(enabled, {0xfb}, &memory) && !run(enabled, {0xdb}, nullptr)
        && !enabled.canAcceptMaskableInterrupt() && enabled.executeInstruction(bytes, nullptr, &io)
        && enabled.canAcceptMaskableInterrupt();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "register_in") return registers(false) ? 0 : 1;
        if (name == "register_out") return registers(true) ? 0 : 1;
        const bool passed = name == "immediate_in" ? immediate(false) : name == "immediate_out" ? immediate(true)
            : name == "missing" && missing();
        if (passed) return 0;
    }
    std::cerr << "Failed Z80 I/O test\n";
    return 1;
}
