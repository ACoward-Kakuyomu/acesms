#pragma once

#include "core/bus/MemoryBus.h"
#include "core/Machine.h"

namespace smsgg {
struct MemoryControlState { bool ioDisabled{}; };

class MemoryControl final : public Z80Io {
public:
    MemoryControl(MemoryBus& memory, Z80Io& devices, Machine machine = Machine::Sms);
    uint8_t readPort(uint16_t port, uint64_t tstate = 0) override;
    void writePort(uint16_t port, uint8_t value, uint64_t tstate = 0) override;
    MemoryControlState captureState() const noexcept { return {ioDisabled_}; }
    void restoreState(const MemoryControlState& state) noexcept { ioDisabled_=state.ioDisabled; }
private:
    MemoryBus& memory_;
    Z80Io& devices_;
    Machine machine_;
    bool ioDisabled_{};
};
} // namespace smsgg
