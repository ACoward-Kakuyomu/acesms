#pragma once

#include "core/cartridge/IMapper.h"
#include "core/cartridge/RomImage.h"

#include <array>

namespace smsgg {
class SegaMapper final : public IMapper {
public:
    explicit SegaMapper(const RomImage& image, std::span<uint8_t> sram = {});
    uint8_t read8(uint16_t address) override;
    void write8(uint16_t address, uint8_t value) override;
    void reset() override;
    MapperState captureState() const override { return SegaMapperState{banks_,control_}; }
    void restoreState(const MapperState& state) override {
        const auto& saved=std::get<SegaMapperState>(state);banks_=saved.banks;control_=saved.control;
    }
private:
    std::span<const uint8_t> rom_;
    std::span<uint8_t> sram_;
    std::array<uint8_t, 3> banks_{0, 1, 2};
    uint8_t control_{};
};
} // namespace smsgg
