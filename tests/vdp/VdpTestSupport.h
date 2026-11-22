#pragma once

#include "core/video/Vdp.h"

namespace smsgg::test {
inline void command(Vdp& vdp, uint16_t address, uint8_t code) {
    vdp.writeControl(static_cast<uint8_t>(address));
    vdp.writeControl(static_cast<uint8_t>((address >> 8) | (code << 6)));
}
inline void reg(Vdp& vdp, uint8_t index, uint8_t value) {
    vdp.writeControl(value); vdp.writeControl(static_cast<uint8_t>(0x80 | index));
}
} // namespace smsgg::test
