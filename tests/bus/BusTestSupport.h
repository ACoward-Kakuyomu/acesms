#pragma once

#include "core/bus/MemoryBus.h"
#include "../cpu/Z80MemoryTestSupport.h"

namespace smsgg::test {
// Records CPU operand accesses while executing every access on the real bus.
class TraceMemory final : public Z80Memory {
public:
    explicit TraceMemory(MemoryBus& bus) : bus_(bus) {}
    std::vector<Memory::Access> accesses;
    uint8_t read8(uint16_t address) override {
        const auto value = bus_.read8(address); accesses.push_back({false, address, value}); return value;
    }
    void write8(uint16_t address, uint8_t value) override {
        accesses.push_back({true, address, value}); bus_.write8(address, value);
    }
private:
    MemoryBus& bus_;
};
} // namespace smsgg::test
