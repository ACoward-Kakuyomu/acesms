#pragma once

#include "core/cartridge/IMapper.h"
#include "core/cartridge/RomImage.h"

#include <memory>
#include <span>

namespace smsgg {
struct CartridgeState {
    std::vector<uint8_t> ram;
    MapperState mapper;
};

class Cartridge final : public Z80Memory {
public:
    explicit Cartridge(RomImage image);
    uint8_t read8(uint16_t address) override;
    void write8(uint16_t address, uint8_t value) override;
    void reset();
    std::span<uint8_t> ram() noexcept { return sram_; }
    CartridgeState captureState() const;
    // Same validated ROM/mapper/capacity; keep the storage referenced by the mapper.
    void restoreState(const CartridgeState& state);
private:
    RomImage image_;
    std::vector<uint8_t> sram_;
    std::unique_ptr<IMapper> mapper_;
};
} // namespace smsgg
