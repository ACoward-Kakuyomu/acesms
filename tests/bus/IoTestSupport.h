#pragma once

#include "core/cpu/Z80.h"

#include <vector>

namespace smsgg::test {
class Io final : public Z80Io {
public:
    struct Access { bool write; uint16_t port; uint8_t value; uint64_t tstate{}; };
    uint8_t reply{};
    std::vector<Access> accesses;
    uint8_t readPort(uint16_t port, uint64_t tstate) override {
        accesses.push_back({false, port, reply, tstate}); return reply;
    }
    void writePort(uint16_t port, uint8_t value, uint64_t tstate) override {
        accesses.push_back({true, port, value, tstate});
    }
};
} // namespace smsgg::test
