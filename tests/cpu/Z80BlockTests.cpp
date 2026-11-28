#include "Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Z80;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
using smsgg::test::Memory;
using smsgg::test::run;

namespace {

bool memptrIs(Z80& cpu, Memory& memory, uint16_t value) {
    memory.bytes[cpu.registers().hl] = 0;
    const auto flags = static_cast<uint8_t>((cpu.registers().f() & 1) | 0x54 | ((value / 256) & 0x28));
    return run(cpu, {0xcb, 0x46}, &memory) && cpu.registers().f() == flags;
}

bool transfer(uint8_t opcode) {
    Memory memory;
    const int direction = (opcode == 0xa8 || opcode == 0xb8) ? -1 : 1;
    const bool repeat = opcode == 0xb0 || opcode == 0xb8;
    const std::array<uint16_t, 4> counts{0, 1, 2, 0xffff};
    const std::array<uint16_t, 3> pcs{0x07ff, 0x2800, 0xffff};
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned data = 0; data < 256; ++data) {
            for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                for (size_t count = 0; count < counts.size(); ++count) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(a * 256 + flags);
                    expected.bc = counts[count];
                    expected.hl = a % 2 == 0 ? 0xffff : 0;
                    expected.de = data % 3 == 0 ? expected.hl : data % 3 == 1 ? 0 : 0xffff;
                    expected.r = static_cast<uint8_t>(data);
                    Z80 cpu(expected);
                    const uint16_t pc = pcs[a % pcs.size()];
                    if (!run(cpu, {0xc3, static_cast<uint8_t>(pc), static_cast<uint8_t>(pc / 256)}, &memory)) return false;
                    expected = cpu.registers();
                    const auto source = expected.hl, target = expected.de;
                    memory.bytes[target] = static_cast<uint8_t>(255 - data);
                    memory.bytes[source] = static_cast<uint8_t>(data);
                    memory.accesses.clear();
                    const bool prefix = count % 2 != 0;
                    const uint16_t start = static_cast<uint16_t>(pc + (prefix ? 2 : 0));
                    const auto elapsed = prefix ? run(cpu, {0xdd, 0xfd, 0xed, opcode}, &memory)
                        : run(cpu, {0xed, opcode}, &memory);
                    expected.hl = static_cast<uint16_t>(source + direction);
                    expected.de = static_cast<uint16_t>(target + direction);
                    expected.bc = static_cast<uint16_t>(counts[count] - 1);
                    const bool again = repeat && expected.bc != 0;
                    const unsigned sum = (a + data) % 256;
                    const unsigned xy = again ? (start / 256) & 0x28 : (sum / 8 % 2) * 8 + (sum / 2 % 2) * 0x20;
                    expected.setF(static_cast<uint8_t>((flags & 0xc1) + xy + (expected.bc != 0 ? 4 : 0)));
                    expected.pc = again ? start : static_cast<uint16_t>(start + 2);
                    const unsigned r = expected.r;
                    expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + (prefix ? 4 : 2)) % 128);
                    if (!elapsed || *elapsed != (again ? 21 : 16) + (prefix ? 8 : 0)
                        || !sameRegisters(cpu.registers(), expected) || memory.bytes[target] != data
                        || memory.accesses.size() != 2 || memory.accesses[0].write || memory.accesses[0].address != source
                        || memory.accesses[0].value != data || !memory.accesses[1].write
                        || memory.accesses[1].address != target || memory.accesses[1].value != data) return false;
                    if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (expected.a() & 0x28)
                        || !memptrIs(cpu, memory, again ? static_cast<uint16_t>(start + 1) : pc)) return false;
                }
            }
        }
    }
    return true;
}

bool series() {
    Memory memory;
    for (const uint16_t count : {3, 0}) {
        auto initial = initialRegisters();
        initial.bc = count; initial.hl = 0; initial.de = 0x4000; initial.pc = 0xffff;
        Z80 cpu(initial);
        const unsigned iterations = count == 0 ? 65536 : count;
        uint64_t total = 0;
        for (unsigned i = 0; i < iterations; ++i) {
            const auto time = i == 0 ? run(cpu, {0xdd, 0xed, 0xb0}, &memory) : run(cpu, {0xed, 0xb0}, &memory);
            if (!time || *time != (i + 1 == iterations ? 16 : 21) + (i == 0 ? 4 : 0)) return false;
            total += *time;
            if (cpu.registers().bc != static_cast<uint16_t>(count - i - 1)
                || cpu.registers().pc != (i + 1 == iterations ? 2 : 0)) return false;
        }
        const unsigned r = initial.r;
        if (total != (iterations - 1) * 21ull + 20 || cpu.registers().hl != static_cast<uint16_t>(iterations)
            || cpu.registers().de != static_cast<uint16_t>(0x4000 + iterations)
            || cpu.registers().r != r / 128 * 128 + (r % 128 + iterations * 2 + 1) % 128) return false;
    }
    return true;
}

bool search(uint8_t opcode) {
    Memory memory;
    const int direction = (opcode & 8) != 0 ? -1 : 1;
    const std::array<uint16_t, 4> counts{0, 1, 2, 0xffff};
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned data = 0; data < 256; ++data) {
            for (const unsigned flags : {0, 1, 0xfe, 0xff}) {
                for (size_t c = 0; c < counts.size(); ++c) {
                    auto expected = initialRegisters();
                    expected.af = static_cast<uint16_t>(a * 256 + flags);
                    expected.bc = counts[c]; expected.hl = a % 2 == 0 ? 0xffff : 0;
                    expected.pc = a % 3 == 0 ? 0x07ff : a % 3 == 1 ? 0x2800 : 0xffff;
                    expected.r = static_cast<uint8_t>(data);
                    Z80 cpu(expected);
                    // JP establishes the initial MEMPTR without exposing private state.
                    if (!run(cpu, {0xc3, static_cast<uint8_t>(expected.pc), static_cast<uint8_t>(expected.pc >> 8)}, &memory)) return false;
                    expected = cpu.registers();
                    const uint16_t source = expected.hl, oldMemptr = expected.pc;
                    memory.bytes[source] = static_cast<uint8_t>(data); memory.accesses.clear();
                    const bool prefix = c % 2 != 0;
                    const uint16_t start = static_cast<uint16_t>(expected.pc + (prefix ? 2 : 0));
                    const auto elapsed = prefix ? run(cpu, {0xfd, 0xdd, 0xed, opcode}, &memory)
                        : run(cpu, {0xed, opcode}, &memory);
                    const unsigned result = (a + 256 - data) % 256;
                    const bool half = a % 16 < data % 16;
                    const unsigned adjusted = (result + 256 - (half ? 1 : 0)) % 256;
                    expected.hl = static_cast<uint16_t>(source + direction); --expected.bc;
                    const bool again = (opcode & 0x10) != 0 && expected.bc != 0 && a != data;
                    const unsigned xy = again ? (start >> 8) & 0x28
                        : (adjusted / 8 % 2) * 8 + (adjusted / 2 % 2) * 0x20;
                    expected.setF(static_cast<uint8_t>((flags & 1) + (result >= 128 ? 0x80 : 0)
                        + (result == 0 ? 0x40 : 0) + (half ? 0x10 : 0)
                        + (expected.bc != 0 ? 4 : 0) + 2 + xy));
                    expected.pc = again ? start : static_cast<uint16_t>(start + 2);
                    const unsigned r = expected.r;
                    expected.r = static_cast<uint8_t>(r / 128 * 128 + (r % 128 + (prefix ? 4 : 2)) % 128);
                    if (!elapsed || *elapsed != (again ? 21 : 16) + (prefix ? 8 : 0)
                        || !sameRegisters(cpu.registers(), expected) || memory.accesses.size() != 1
                        || memory.accesses[0].write || memory.accesses[0].address != source
                        || memory.accesses[0].value != data) return false;
                    if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (a & 0x28)
                        || !memptrIs(cpu, memory, again ? static_cast<uint16_t>(start + 1)
                            : static_cast<uint16_t>(oldMemptr + direction))) return false;
                }
            }
        }
    }
    return true;
}

bool searchSeries() {
    Memory memory;
    for (const bool reverse : {false, true}) {
        for (const bool match : {false, true}) {
            auto initial = initialRegisters();
            initial.setA(0x80); initial.bc = 3; initial.hl = reverse ? 0 : 0xfffe; initial.pc = 0xffff;
            Z80 cpu(initial);
            for (unsigned i = 0; i < 3; ++i) {
                memory.bytes[cpu.registers().hl] = match && i == 1 ? 0x80 : 0;
                const auto time = run(cpu, {0xed, static_cast<uint8_t>(reverse ? 0xb9 : 0xb1)}, &memory);
                const bool finish = match ? i == 1 : i == 2;
                if (!time || *time != (finish ? 16 : 21) || cpu.registers().bc != 2 - i
                    || cpu.registers().pc != (finish ? 1 : 0xffff)) return false;
                if (finish) break;
            }
        }
    }
    return true;
}

bool memptrSteps() {
    Memory memory;
    for (const uint16_t seed : {0x07ff, 0x0800, 0x1fff, 0x2000, 0x27ff, 0x2800, 0xffff, 0}) {
        for (const int direction : {-1, 1}) {
            Z80 cpu(initialRegisters());
            if (!run(cpu, {0xc3, static_cast<uint8_t>(seed), static_cast<uint8_t>(seed >> 8)}, &memory)) return false;
            for (unsigned i = 1; i <= 257; ++i) {
                if (!run(cpu, {0xed, static_cast<uint8_t>(direction > 0 ? 0xa1 : 0xa9)}, &memory)
                    || !memptrIs(cpu, memory, static_cast<uint16_t>(seed + direction * static_cast<int>(i)))) return false;
            }
        }
    }
    return true;
}

class BlockIo final : public smsgg::Z80Io {
public:
    explicit BlockIo(const Memory& memory) : memory_(memory) {}
    uint8_t reply{};
    unsigned calls{};
    bool write{};
    uint16_t port{};
    uint8_t data{};
    size_t memoryAccesses{};
    uint8_t readPort(uint16_t address, uint64_t) override {
        ++calls; write = false; port = address; data = reply;
        memoryAccesses = memory_.accesses.size(); return reply;
    }
    void writePort(uint16_t address, uint8_t value, uint64_t) override {
        ++calls; write = true; port = address; data = value;
        memoryAccesses = memory_.accesses.size();
    }
private:
    const Memory& memory_;
};

bool parity(unsigned value) {
    unsigned ones = 0;
    for (; value != 0; value /= 2) ones += value % 2;
    return ones % 2 == 0;
}

bool blockIo(uint8_t opcode) {
    Memory memory;
    BlockIo io(memory);
    const bool output = (opcode & 1) != 0;
    const int direction = (opcode & 8) != 0 ? -1 : 1;
    for (unsigned b = 0; b < 256; ++b) {
        for (unsigned data = 0; data < 256; ++data) {
            for (const unsigned c : {0u, 1u, 7u, 0x7fu, 0x80u, 0xfeu, 0xffu, data}) {
                auto expected = initialRegisters();
                expected.bc = static_cast<uint16_t>(b * 256 + c);
                expected.hl = static_cast<uint16_t>((b % 2 == 0 ? 0xff00 : 0) + (data ^ b));
                expected.setF(static_cast<uint8_t>(data ^ c));
                expected.pc = data % 3 == 0 ? 0x07ff : data % 3 == 1 ? 0x2800 : 0xffff;
                expected.r = static_cast<uint8_t>(data);
                Z80 cpu(expected);
                const uint16_t source = expected.hl;
                memory.bytes[source] = static_cast<uint8_t>(output ? data : 255 - data);
                memory.accesses.clear(); io.calls = 0; io.reply = static_cast<uint8_t>(data);
                const bool prefix = c % 2 != 0;
                const uint16_t start = static_cast<uint16_t>(expected.pc + (prefix ? 2 : 0));
                const std::array<uint8_t, 4> bytes{0xdd, 0xfd, 0xed, opcode};
                const auto time = cpu.executeInstruction(std::span<const uint8_t>(bytes).subspan(prefix ? 0 : 2), &memory, &io);
                expected.setB(static_cast<uint8_t>((b + 255) % 256));
                expected.hl = static_cast<uint16_t>(expected.hl + direction);
                const unsigned newB = expected.b();
                const unsigned addend = output ? expected.l() : (c + 256 + direction) % 256;
                const unsigned k = data + addend;
                const bool carry = k > 255, negative = data >= 128;
                bool half = carry, pv = parity((k % 8) ^ newB);
                const bool again = (opcode & 0x10) != 0 && newB != 0;
                if (again) {
                    const unsigned v = carry ? (newB + (negative ? 255 : 1)) % 256 : newB;
                    pv = pv != !parity(v % 8);
                    if (carry) half = newB % 16 == (negative ? 0 : 15);
                }
                const unsigned xy = again ? (start >> 8) & 0x28 : newB & 0x28;
                expected.setF(static_cast<uint8_t>((newB >= 128 ? 0x80 : 0) + (newB == 0 ? 0x40 : 0)
                    + xy + (half ? 0x10 : 0) + (pv ? 4 : 0) + (negative ? 2 : 0) + (carry ? 1 : 0)));
                expected.pc = again ? start : static_cast<uint16_t>(start + 2);
                expected.r = static_cast<uint8_t>(data / 128 * 128 + (data % 128 + (prefix ? 4 : 2)) % 128);
                const uint16_t port = static_cast<uint16_t>((output ? newB : b) * 256 + c);
                if (!time || *time != (again ? 21 : 16) + (prefix ? 8 : 0)
                    || !sameRegisters(cpu.registers(), expected) || io.calls != 1 || io.write != output
                    || io.port != port || io.data != data || io.memoryAccesses != (output ? 1 : 0)
                    || !memory.accessedOnce(!output, source, static_cast<uint8_t>(data))) return false;
                if (!run(cpu, {0x37}, &memory) || (cpu.registers().f() & 0x28) != (expected.a() & 0x28)
                    || !memptrIs(cpu, memory, static_cast<uint16_t>(port + direction))) return false;
            }
        }
    }
    return true;
}

bool ioSeries() {
    Memory memory;
    BlockIo io(memory);
    for (const uint8_t opcode : {0xb2, 0xba, 0xb3, 0xbb}) {
        for (const unsigned count : {0, 1, 3}) {
            auto initial = initialRegisters(); initial.setB(static_cast<uint8_t>(count)); initial.pc = 0xffff;
            Z80 cpu(initial);
            const unsigned iterations = count == 0 ? 256 : count;
            uint64_t total = 0;
            for (unsigned i = 0; i < iterations; ++i) {
                const std::array<uint8_t, 3> bytes{0xdd, 0xed, opcode};
                const auto time = cpu.executeInstruction(std::span<const uint8_t>(bytes).subspan(i == 0 ? 0 : 1), &memory, &io);
                if (!time || *time != (i + 1 == iterations ? 16 : 21) + (i == 0 ? 4 : 0)
                    || cpu.registers().b() != (count + 255 - i) % 256
                    || cpu.registers().pc != (i + 1 == iterations ? 2 : 0)) return false;
                total += *time;
            }
            if (total != (iterations - 1) * 21ull + 20 || io.calls != iterations) return false;
            io.calls = 0;
        }
    }
    return true;
}

bool ioMissing() {
    Memory memory;
    BlockIo io(memory);
    for (const uint8_t opcode : {0xa2, 0xaa, 0xb2, 0xba, 0xa3, 0xab, 0xb3, 0xbb}) {
        for (const bool provideMemory : {false, true}) {
            Z80 cpu(initialRegisters());
            if (!run(cpu, {0xfb}, &memory)) return false;
            const auto before = cpu.registers();
            const std::array<uint8_t, 3> bytes{0xfd, 0xed, opcode};
            if (cpu.executeInstruction(bytes, provideMemory ? &memory : nullptr, provideMemory ? nullptr : &io)
                || !sameRegisters(cpu.registers(), before) || io.calls != 0 || !memory.accesses.empty()
                || cpu.canAcceptMaskableInterrupt()) return false;
            if (!cpu.executeInstruction(bytes, &memory, &io) || !cpu.canAcceptMaskableInterrupt()) return false;
            memory.accesses.clear(); io.calls = 0;
        }
    }
    return true;
}

bool missing() {
    Memory memory;
    for (const uint8_t opcode : {0xa0, 0xa8, 0xb0, 0xb8, 0xa1, 0xa9, 0xb1, 0xb9}) {
        Z80 cpu(initialRegisters());
        const auto before = cpu.registers();
        if (run(cpu, {0xed, opcode}, nullptr) || run(cpu, {0xdd, 0xed, opcode}, nullptr)
            || !sameRegisters(cpu.registers(), before) || !memory.accesses.empty()) return false;
    }
    Z80 enabled(initialRegisters());
    return run(enabled, {0xfb}, &memory) && !run(enabled, {0xed, 0xb0}, nullptr)
        && !enabled.canAcceptMaskableInterrupt() && run(enabled, {0xed, 0xb0}, &memory)
        && enabled.canAcceptMaskableInterrupt();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "ldi") return transfer(0xa0) ? 0 : 1;
        if (name == "ldd") return transfer(0xa8) ? 0 : 1;
        if (name == "ldir") return transfer(0xb0) ? 0 : 1;
        if (name == "lddr") return transfer(0xb8) ? 0 : 1;
        if (name == "series") return series() ? 0 : 1;
        if (name == "missing") return missing() ? 0 : 1;
        if (name == "cpi") return search(0xa1) ? 0 : 1;
        if (name == "cpd") return search(0xa9) ? 0 : 1;
        if (name == "cpir") return search(0xb1) ? 0 : 1;
        if (name == "cpdr") return search(0xb9) ? 0 : 1;
        if (name == "search_series") return searchSeries() ? 0 : 1;
        if (name == "memptr") return memptrSteps() ? 0 : 1;
        if (name == "ini") return blockIo(0xa2) ? 0 : 1;
        if (name == "ind") return blockIo(0xaa) ? 0 : 1;
        if (name == "inir") return blockIo(0xb2) ? 0 : 1;
        if (name == "indr") return blockIo(0xba) ? 0 : 1;
        if (name == "outi") return blockIo(0xa3) ? 0 : 1;
        if (name == "outd") return blockIo(0xab) ? 0 : 1;
        if (name == "otir") return blockIo(0xb3) ? 0 : 1;
        if (name == "otdr") return blockIo(0xbb) ? 0 : 1;
        if (name == "io_series") return ioSeries() ? 0 : 1;
        if (name == "io_missing") return ioMissing() ? 0 : 1;
    }
    std::cerr << "Failed Z80 block test\n";
    return 1;
}
