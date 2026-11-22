#pragma once

#include "core/cartridge/IMapper.h"
#include "core/cartridge/RomImage.h"

#include <array>

namespace smsgg {
class CodemastersMapper final : public IMapper {
public:
    explicit CodemastersMapper(const RomImage& image,std::span<uint8_t> ram = {}) : rom_(image.bytes()),ram_(ram) {}
    uint8_t read8(uint16_t address) override;
    void write8(uint16_t address,uint8_t value) override;
    void reset() override;
    MapperState captureState() const override { return CodemastersMapperState{banks_,ramEnabled_}; }
    void restoreState(const MapperState& state) override {
        const auto& saved=std::get<CodemastersMapperState>(state);banks_=saved.banks;ramEnabled_=saved.ramEnabled;
    }
private:
    std::span<const uint8_t> rom_;
    std::span<uint8_t> ram_;
    std::array<uint8_t,3> banks_{0,1,0};
    bool ramEnabled_{};
};
} // namespace smsgg
