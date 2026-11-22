#include "core/bus/MemoryBus.h"

namespace smsgg {

MemoryBus::MemoryBus(Z80Memory& externalMemory, Machine machine)
    : externalMemory_(externalMemory), machine_(machine) {}

uint8_t MemoryBus::read8(uint16_t address) {
    if (address >= 0xc000) {
        return ramEnabled_ ? ram_[address & ramMask()] : 0xff;
    }
    return cartridgeEnabled_ ? externalMemory_.read8(address) : 0xff;
}

void MemoryBus::write8(uint16_t address, uint8_t value) {
    if (address >= 0xc000) {
        if (ramEnabled_) ram_[address & ramMask()] = value;
        if (machine_ == Machine::Sg1000) return;
    }
    if (cartridgeEnabled_) externalMemory_.write8(address, value);
}

void MemoryBus::setMemoryControl(uint8_t value) {
    if (machine_ == Machine::Sg1000) return;
    ramEnabled_ = (value & 0x10) == 0;
    cartridgeEnabled_ = (value & 0x40) == 0;
}

void MemoryBus::writeWorkRam(size_t offset, unsigned width, uint32_t value) noexcept {
    for (unsigned byte = 0; byte < width; ++byte) {
        ram_[offset + byte] = static_cast<uint8_t>(value >> (8 * byte));
    }
}

} // namespace smsgg
