#include "core/cartridge/SegaMapper.h"

namespace smsgg {
SegaMapper::SegaMapper(const RomImage& image, std::span<uint8_t> sram) : rom_(image.bytes()), sram_(sram) {}

uint8_t SegaMapper::read8(uint16_t address) {
    if (address >= 0xc000) return 0xff;
    if (address < 0x400) return rom_[address];
    if (address >= 0x8000 && (control_ & 8) != 0) {
        if (sram_.empty()) return 0xff;
        const size_t index = ((control_ & 4) != 0 ? 0x4000 : 0) + address - 0x8000;
        return sram_[index & (sram_.size() - 1)];
    }
    const auto bank = banks_[address >> 14];
    const auto offset = address & 0x3fff;
    if (rom_.size() == segaRomSize768KiB) return rom_[(bank % 48) * 0x4000 + offset];
    return rom_[(bank * 0x4000 + offset) & (rom_.size() - 1)];
}

void SegaMapper::write8(uint16_t address, uint8_t value) {
    if (address >= 0xfffd) banks_[address - 0xfffd] = value;
    else if (address == 0xfffc) control_ = value;
    else if (address >= 0x8000 && address < 0xc000 && (control_ & 8) != 0 && !sram_.empty()) {
        const size_t index = ((control_ & 4) != 0 ? 0x4000 : 0) + address - 0x8000;
        sram_[index & (sram_.size() - 1)] = value;
    }
}

void SegaMapper::reset() { banks_ = {0, 1, 2}; control_ = 0; }
} // namespace smsgg
