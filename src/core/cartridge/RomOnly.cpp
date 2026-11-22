#include "core/cartridge/RomOnly.h"

namespace smsgg {
RomOnly::RomOnly(const RomImage& image)
    : rom_(image.bytes()), sg1000_(image.options().machine == Machine::Sg1000) {}

uint8_t RomOnly::read8(uint16_t address) {
    if (address >= 0xc000) return 0xff;
    if (sg1000_ && address >= 0x8000) {
        if (rom_.size() <= 0x8000) return 0xff;
        return rom_[0x8000 + (address - 0x8000) % (rom_.size() - 0x8000)];
    }
    return rom_[address % rom_.size()];
}

void RomOnly::write8(uint16_t, uint8_t) {}
void RomOnly::reset() {}
} // namespace smsgg
