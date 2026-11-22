#include "Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>
#include <variant>

namespace {
class Io final : public smsgg::Z80Io {
public:
    uint8_t readPort(uint16_t) override { return 0xff; }
    void writePort(uint16_t, uint8_t, uint64_t) override {}
};

bool basic() {
    smsgg::test::Memory memory;
    Io io;
    smsgg::Z80 cpu({.pc = 0x1000, .r = 0xff});
    memory.bytes[0x1000] = 0x00;
    memory.bytes[0x1001] = 0x3e;
    memory.bytes[0x1002] = 0x42;
    const auto nop = cpu.step(memory, io);
    if (std::get<uint32_t>(nop) != 4 || cpu.registers().pc != 0x1001
        || cpu.registers().r != 0x80 || !memory.accessedOnce(false, 0x1000, 0)) return false;
    memory.accesses.clear();
    const auto load = cpu.step(memory, io);
    return std::get<uint32_t>(load) == 7 && cpu.registers().a() == 0x42
        && cpu.registers().pc == 0x1003 && cpu.registers().r == 0x81
        && memory.accesses.size() == 2 && memory.accesses[0].address == 0x1001
        && memory.accesses[1].address == 0x1002;
}

bool compare(std::vector<uint8_t> bytes, uint16_t pc = 0x1000) {
    smsgg::test::Memory fetched, supplied;
    Io io;
    auto registers = smsgg::test::initialRegisters();
    registers.pc = pc;
    for (size_t i = 0; i < bytes.size(); ++i)
        fetched.bytes[static_cast<uint16_t>(pc + i)] = bytes[i];
    supplied.bytes = fetched.bytes;
    smsgg::Z80 a(registers), b(registers);
    const auto actual = a.step(fetched, io);
    const auto expected = b.executeInstruction(bytes, &supplied, &io);
    size_t prefixCount=0;
    while(prefixCount<bytes.size() && (bytes[prefixCount]==0xdd || bytes[prefixCount]==0xfd)) ++prefixCount;
    if (!expected || !std::holds_alternative<uint32_t>(actual)
        || std::get<uint32_t>(actual) != *expected
        || *expected>prefixCount*4+64 // System PCM scratch bound, including indexed/ED bodies.
        || !smsgg::test::sameRegisters(a.registers(), b.registers())
        || a.iff1() != b.iff1() || a.iff2() != b.iff2()
        || a.halted() != b.halted() || a.interruptMode() != b.interruptMode()
        || fetched.bytes != supplied.bytes
        || fetched.accesses.size() != bytes.size() + supplied.accesses.size()) {
        std::cerr << "Mismatch at opcode " << std::hex << unsigned(bytes[0]) << '\n';
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i)
        if (fetched.accesses[i].write || fetched.accesses[i].address != static_cast<uint16_t>(pc + i)
            || fetched.accesses[i].value != bytes[i]) return false;
    for (size_t i = 0; i < supplied.accesses.size(); ++i) {
        const auto x = fetched.accesses[bytes.size() + i], y = supplied.accesses[i];
        if (x.write != y.write || x.address != y.address || x.value != y.value) return false;
    }
    // A further instruction also observes private EI-delay/flag-latch state.
    const std::array<uint8_t, 1> ccf{0x3f};
    return a.executeInstruction(ccf, &fetched, &io) == b.executeInstruction(ccf, &supplied, &io)
        && smsgg::test::sameRegisters(a.registers(), b.registers())
        && a.canAcceptMaskableInterrupt() == b.canAcceptMaskableInterrupt();
}

unsigned baseLength(unsigned opcode) {
    // Independent transcription of the Z80 opcode grid; prefixes handled below.
    constexpr std::string_view low[]{
        "1311112111111121", "2311112121111121",
        "2331112121311121", "2331112121311121"};
    constexpr std::string_view high[]{
        "1133312111323321", "1132312111323121",
        "1131312111313121", "1131312111313121"};
    return opcode < 0x40 ? low[opcode / 16][opcode % 16] - '0'
        : opcode >= 0xc0 ? high[(opcode - 0xc0) / 16][opcode % 16] - '0' : 1;
}

bool opcodes() {
    for (unsigned op = 0; op < 256; ++op) {
        if (op == 0xcb || op == 0xed || op == 0xdd || op == 0xfd) continue;
        std::vector<uint8_t> bytes(baseLength(op), 0x42);
        bytes[0] = static_cast<uint8_t>(op);
        if (!compare(bytes)) return false;
    }
    for (unsigned op = 0; op < 256; ++op) {
        if (!compare({0xcb, static_cast<uint8_t>(op)})) return false;
        const bool word = (op & 0xc7) == 0x43;
        std::vector<uint8_t> bytes{0xed, static_cast<uint8_t>(op)};
        if (word) { bytes.push_back(0x42); bytes.push_back(0x80); }
        if (!compare(bytes)) return false;
    }
    return true;
}

bool prefixes() {
    for (uint8_t prefix : {0xdd, 0xfd}) {
        for (unsigned op = 0; op < 256; ++op) {
            if (op == 0xcb || op == 0xed || op == 0xdd || op == 0xfd) continue;
            const bool displacement = op == 0x34 || op == 0x35 || op == 0x36
                || (op >= 0x80 && op <= 0xbf && op % 8 == 6)
                || (op >= 0x40 && op <= 0x7f && op != 0x76
                    && (op % 8 == 6 || op / 8 % 8 == 6));
            std::vector<uint8_t> bytes{prefix, static_cast<uint8_t>(op)};
            bytes.resize(1 + baseLength(op) + displacement, 0xfe);
            if (!compare(bytes, 0xffff)) return false;
        }
        for (unsigned op = 0; op < 256; ++op) {
            if (!compare({prefix, 0xcb, 0xfe, static_cast<uint8_t>(op)})) return false;
            std::vector<uint8_t> bytes{prefix, 0xed, static_cast<uint8_t>(op)};
            if ((op & 0xc7) == 0x43) { bytes.push_back(0x42); bytes.push_back(0x80); }
            if (!compare(bytes)) return false;
        }
    }
    return compare({0xdd, 0xfd, 0xdd, 0x21, 0x34, 0x12}, 0xfffd)
        && compare({0xfd, 0xdd, 0xfd, 0x36, 0xff, 0x5a}, 0xfffc)
        && compare({0x3e, 0x12}, 0xffff) && compare({0xc3, 0x34, 0x12}, 0xffff);
}

bool haltAndLimit() {
    smsgg::test::Memory memory;
    Io io;
    smsgg::Z80 cpu({});
    memory.bytes[0] = 0x76;
    if (std::get<uint32_t>(cpu.step(memory, io)) != 4 || !cpu.halted()) return false;
    memory.accesses.clear();
    if (std::get<uint32_t>(cpu.step(memory, io)) != 4 || cpu.registers().pc != 1
        || cpu.registers().r != 2 || !memory.accesses.empty()) return false;
    const auto before = smsgg::test::initialRegisters();
    smsgg::Z80 looping(before);
    for (size_t i = 0; i < memory.bytes.size(); ++i) memory.bytes[i] = i % 2 ? 0xfd : 0xdd;
    const auto result = looping.step(memory, io);
    if (std::get<smsgg::CpuStepError>(result) != smsgg::CpuStepError::PrefixRun
        || memory.accesses.size() != 65536
        || !smsgg::test::sameRegisters(looping.registers(), before)
        || looping.iff1() || looping.halted()) return false;
    // The largest supported run still executes the terminating NOP after wrapping PC.
    memory.accesses.clear();
    memory.bytes[static_cast<uint16_t>(before.pc - 1)] = 0;
    const auto boundary = looping.step(memory, io);
    return std::get<uint32_t>(boundary) == 262144 && memory.accesses.size() == 65536
        && looping.registers().pc == before.pc && looping.registers().r == before.r;
}
}

int main(int argc, char** argv) {
    const std::string_view name = argc == 2 ? argv[1] : "";
    const bool pass = name == "basic" ? basic() : name == "opcodes" ? opcodes()
        : name == "prefixes" ? prefixes() : name == "halt_limit" && haltAndLimit();
    if (!pass) std::cerr << "Z80 fetch test failed\n";
    return pass ? 0 : 1;
}
