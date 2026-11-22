#pragma once

#include "core/video/Vdp.h"

namespace smsgg {
class Tms9918Renderer final {
public:
    static constexpr uint16_t vramAddress(unsigned address, uint8_t control) {
        if (control & 0x80) return static_cast<uint16_t>(address);
        // 4 KiB DRAM mode replaces address bit 6 with the high column bit.
        return static_cast<uint16_t>((address & 0x203f) | ((address & 0x0fc0) << 1) | ((address & 0x1000) >> 6));
    }

    static uint8_t renderLine(std::span<const uint8_t, 0x4000> vram,
        std::span<const uint8_t, 11> registers, unsigned line, std::span<Rgba8, 256> output);
};
} // namespace smsgg
