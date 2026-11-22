#pragma once

#include "core/Machine.h"
#include "core/cpu/Z80.h"

#include <array>
#include <span>

namespace smsgg {

struct MemoryBusState {
    std::array<uint8_t,0x2000> ram{};
    bool ramEnabled{true},cartridgeEnabled{true};
};

class MemoryBus final : public Z80Memory {
public:
    explicit MemoryBus(Z80Memory& externalMemory, Machine machine = Machine::Sms);
    uint8_t read8(uint16_t address) override;
    void write8(uint16_t address, uint8_t value) override;
    void setMemoryControl(uint8_t value);
    std::span<const uint8_t> workRam() const noexcept { return std::span(ram_).first(ramMask() + 1); }
    // Caller validates the physical RAM offset and value width.
    void writeWorkRam(size_t offset, unsigned width, uint32_t value) noexcept;
    MemoryBusState captureState() const noexcept { return {ram_,ramEnabled_,cartridgeEnabled_}; }
    void restoreState(const MemoryBusState& state) noexcept {
        ram_=state.ram;ramEnabled_=state.ramEnabled;cartridgeEnabled_=state.cartridgeEnabled;
    }
private:
    size_t ramMask() const noexcept { return workRamBytes(machine_) - 1; }
    std::array<uint8_t, 0x2000> ram_{};
    Z80Memory& externalMemory_;
    const Machine machine_;
    bool ramEnabled_{true};
    bool cartridgeEnabled_{true};
};

} // namespace smsgg
