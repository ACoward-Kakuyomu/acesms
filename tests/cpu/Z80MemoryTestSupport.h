#pragma once

#include "Z80TestSupport.h"

#include <array>
#include <initializer_list>
#include <span>
#include <vector>

namespace smsgg::test {

class Memory final : public Z80Memory {
public:
    struct Access { bool write; uint16_t address; uint8_t value; };
    std::array<uint8_t, 65536> bytes{};
    std::vector<Access> accesses;
    uint8_t read8(uint16_t address) override {
        accesses.push_back({false, address, bytes[address]});
        return bytes[address];
    }
    void write8(uint16_t address, uint8_t value) override {
        accesses.push_back({true, address, value});
        bytes[address] = value;
    }
    bool accessedOnce(bool write, uint16_t address, uint8_t value) const {
        return accesses.size() == 1 && accesses[0].write == write
            && accesses[0].address == address && accesses[0].value == value;
    }
};

inline std::optional<uint32_t> run(Z80& cpu, std::initializer_list<uint8_t> bytes, Z80Memory* memory) {
    return cpu.executeInstruction(std::span<const uint8_t>(bytes.begin(), bytes.size()), memory);
}

} // namespace smsgg::test
