#pragma once

#include "core/video/Vdp.h"

namespace smsgg {
class Mode4Renderer final {
public:
    static bool spriteOverflow(std::span<const uint8_t, 0x4000> vram, std::span<const uint8_t, 11> registers, unsigned line);
    static uint8_t renderLine(std::span<const uint8_t, 0x4000> vram, std::span<const Rgba8, 32> palette,
        std::span<const uint8_t, 11> registers, uint8_t scrollX, uint8_t scrollY, unsigned line, std::span<Rgba8, 256> output,
        std::span<const CramChange> changes = {});
};
} // namespace smsgg
