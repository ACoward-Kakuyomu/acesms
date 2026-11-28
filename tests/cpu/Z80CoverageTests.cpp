#include "Z80MemoryTestSupport.h"

#include <iostream>

using smsgg::Z80;
using smsgg::test::Memory;
using smsgg::test::initialRegisters;
using smsgg::test::run;

namespace {
class Io final : public smsgg::Z80Io {
public:
    uint8_t readPort(uint16_t, uint64_t) override { return 0; }
    void writePort(uint16_t, uint8_t, uint64_t) override {}
};
} // namespace

int main() {
    Memory memory;
    Io io;
    const std::vector<std::vector<uint8_t>> heads{{}, {0xdd}, {0xfd}, {0xdd, 0xfd}, {0xfd, 0xdd}};
    size_t cases = 0;
    for (const auto& head : heads) {
        for (unsigned opcode = 0; opcode < 256; ++opcode) {
            const bool extended = opcode == 0xcb || opcode == 0xed;
            for (unsigned sub = 0; sub < (extended ? 256u : 1u); ++sub) {
                auto bytes = head;
                bytes.push_back(static_cast<uint8_t>(opcode));
                if (opcode == 0xcb && !head.empty()) bytes.push_back(0xff); // indexed displacement
                bytes.push_back(static_cast<uint8_t>(sub));
                bytes.insert(bytes.end(), 4, 0); // enough operands, including ED nn
                for (const bool interrupt : {false, true}) {
                    for (const uint8_t flags : {0, 0xff}) {
                        auto initial = initialRegisters(); initial.pc = 0xffff; initial.setF(flags);
                        Z80 cpu(initial);
                        if (interrupt && (!run(cpu, {0xfb}, &memory) || !run(cpu, {0}, &memory))) return 1;
                        const auto time = interrupt ? cpu.acceptInterrupt(bytes, &memory, &io)
                            : cpu.executeInstruction(bytes, &memory, &io);
                        if (!time || *time == 0) {
                            std::cerr << "Uncovered opcode=" << opcode << " sub=" << sub
                                << " prefix count=" << head.size() << " IM0=" << interrupt << '\n';
                            return 1;
                        }
                        memory.accesses.clear();
                        ++cases;
                    }
                }
            }
        }
    }
    std::cout << cases << " complete instruction inputs accepted\n";
    return 0;
}
