#include "core/bus/IoBus.h"

#include <array>

namespace smsgg {

IoBus::IoBus(Z80Io& devices, Machine machine, bool fmEnabled)
    : devices_(devices), machine_(machine), fmEnabled_(fmEnabled) {}

uint8_t IoBus::readPort(uint16_t port, uint64_t tstate) {
    const auto low = static_cast<uint8_t>(port);
    if (machine_ == Machine::Sg1000) {
        if (low < 0x80) return 0xff;
        return devices_.readPort(static_cast<uint16_t>((low < 0xc0 ? 0xbe : 0xdc) | (low & 1)), tstate);
    }
    if (fmEnabled_ && low == 0xf2) return devices_.readPort(low, tstate);
    if (machine_ == Machine::GameGear) {
        if (low < 7) return devices_.readPort(low, tstate);
        if (low < 0x40 || (low >= 0xc0 && low != 0xc0 && low != 0xc1 && low != 0xdc && low != 0xdd)) return 0xff;
    }
    constexpr std::array<uint8_t, 4> representatives{0x3e, 0x7e, 0xbe, 0xdc};
    return devices_.readPort(static_cast<uint16_t>(representatives[low >> 6] | (low & 1)), tstate);
}

void IoBus::writePort(uint16_t port, uint8_t value, uint64_t tstate) {
    const auto low = static_cast<uint8_t>(port);
    if (machine_ == Machine::Sg1000) {
        if (low >= 0x40 && low < 0x80) devices_.writePort(0x7f, value, tstate);
        else if (low >= 0x80 && low < 0xc0) devices_.writePort(static_cast<uint16_t>(0xbe | (low & 1)), value, tstate);
        return;
    }
    if (fmEnabled_ && low >= 0xf0 && low <= 0xf2) devices_.writePort(low, value, tstate);
    else if (machine_ == Machine::GameGear && low < 7) devices_.writePort(low, value, tstate);
    else if (low < 0x40) devices_.writePort(static_cast<uint16_t>(0x3e | (low & 1)), value, tstate);
    else if (low < 0x80) devices_.writePort(0x7f, value, tstate);
    else if (low < 0xc0) devices_.writePort(static_cast<uint16_t>(0xbe | (low & 1)), value, tstate);
}

} // namespace smsgg
