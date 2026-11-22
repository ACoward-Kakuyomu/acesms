#pragma once

#include "core/cartridge/IMapper.h"
#include "core/cartridge/RomImage.h"

namespace smsgg {
class RomOnly final : public IMapper {
public:
    explicit RomOnly(const RomImage& image);
    uint8_t read8(uint16_t address) override;
    void write8(uint16_t address, uint8_t value) override;
    void reset() override;
    MapperState captureState() const override { return std::monostate{}; }
    void restoreState(const MapperState&) override {}
private:
    std::span<const uint8_t> rom_;
    bool sg1000_;
};
} // namespace smsgg
