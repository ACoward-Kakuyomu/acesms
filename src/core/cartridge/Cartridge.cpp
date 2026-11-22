#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomOnly.h"
#include "core/cartridge/SegaMapper.h"
#include "core/cartridge/CodemastersMapper.h"

#include <algorithm>

namespace smsgg {
Cartridge::Cartridge(RomImage image) : image_(std::move(image)), sram_(image_.options().sramBytes) {
    if (image_.options().mapper == MapperKind::RomOnly) mapper_ = std::make_unique<RomOnly>(image_);
    else if (image_.options().mapper == MapperKind::Sega) mapper_ = std::make_unique<SegaMapper>(image_, sram_);
    else if (image_.options().mapper == MapperKind::Codemasters) mapper_ = std::make_unique<CodemastersMapper>(image_,sram_);
}

uint8_t Cartridge::read8(uint16_t address) { return mapper_->read8(address); }
void Cartridge::write8(uint16_t address, uint8_t value) { mapper_->write8(address, value); }
void Cartridge::reset() { mapper_->reset(); }
CartridgeState Cartridge::captureState() const { return {sram_,mapper_->captureState()}; }
void Cartridge::restoreState(const CartridgeState& state) {
    std::copy(state.ram.begin(),state.ram.end(),sram_.begin());
    mapper_->restoreState(state.mapper);
}
} // namespace smsgg
