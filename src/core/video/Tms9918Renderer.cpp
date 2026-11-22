#include "core/video/Tms9918Renderer.h"

#include <algorithm>

namespace smsgg {
namespace {
constexpr std::array<Rgba8, 16> colors{{
    {0, 0, 0}, {0, 0, 0}, {33, 200, 66}, {94, 220, 120}, {84, 85, 237}, {125, 118, 252},
    {212, 82, 77}, {66, 235, 245}, {252, 85, 84}, {255, 121, 120}, {212, 193, 84},
    {230, 206, 128}, {33, 176, 59}, {201, 91, 186}, {204, 204, 204}, {255, 255, 255},
}};
}

uint8_t Tms9918Renderer::renderLine(std::span<const uint8_t, 0x4000> vram,
    std::span<const uint8_t, 11> registers, unsigned line, std::span<Rgba8, 256> output) {
    const unsigned backdrop = registers[7] & 15;
    const unsigned mode = (registers[0] & 2) | (registers[1] & 0x18);
    if (!(registers[1] & 0x40) || (mode != 0 && mode != 2 && mode != 8 && mode != 16)) {
        std::ranges::fill(output, colors[backdrop]);
        return 0;
    }
    const auto read = [&](unsigned address) { return vram[vramAddress(address, registers[1])]; };
    const bool text = mode == 16;
    const unsigned columns = text ? 40 : 32;
    const unsigned width = text ? 6 : 8;
    const unsigned left = text ? 6 : 0;
    if (text) {
        std::ranges::fill(output.first<6>(), colors[backdrop]);
        std::ranges::fill(output.last<10>(), colors[backdrop]);
    }
    const unsigned names = (registers[2] & 15) * 0x400 + (line / 8) * columns;
    const unsigned patterns = (registers[4] & 7) * 0x800;
    const unsigned colorTable = registers[3] * 0x40;
    const unsigned colorMask = ((registers[3] & 0x7f) << 3) | 7;
    const unsigned patternMask = ((registers[4] & 3) << 8) | (colorMask & 0xff);
    for (unsigned col = 0; col < columns; ++col) {
        const unsigned name = read(names + col);
        uint8_t pattern, color;
        switch (mode) {
        case 2: { // Graphics II
            const unsigned tile = name + (line / 64) * 256;
            pattern = read((patterns & 0x2000) + (tile & patternMask) * 8 + line % 8);
            color = read((colorTable & 0x2000) + (tile & colorMask) * 8 + line % 8);
            break;
        }
        case 8: // Multicolor
            pattern = 0xf0;
            color = read(patterns + name * 8 + (line / 4) % 8);
            break;
        default: // Graphics I or Text
            pattern = read(patterns + name * 8 + line % 8);
            color = text ? registers[7] : read(colorTable + name / 8);
            break;
        }
        for (unsigned dot = 0; dot < width; ++dot) {
            const unsigned index = (pattern & (0x80 >> dot)) ? color >> 4 : color & 15;
            output[left + col * width + dot] = colors[index != 0 ? index : backdrop];
        }
    }

    if (text) return 0;
    const unsigned attributes = (registers[5] & 0x7f) * 0x80;
    const unsigned spritePatterns = (registers[6] & 7) * 0x800;
    const bool large = (registers[1] & 2) != 0;
    const unsigned scale = (registers[1] & 1) ? 2 : 1;
    const unsigned size = large ? 16 : 8;
    // Early Clock and 2x magnification can move pattern bits outside the active area.
    std::array<uint8_t, 32 + 256 + 32> occupied{};
    unsigned selected = 0;
    uint8_t status = 0;
    for (unsigned index = 0; index < 32; ++index) {
        const unsigned entry = attributes + index * 4;
        const int y = read(entry);
        if (y == 0xd0) break;
        const int top = (y > 0xe0 ? y - 256 : y) + 1;
        const int offset = static_cast<int>(line) - top;
        if (offset < 0 || static_cast<unsigned>(offset) >= size * scale) continue;
        if (selected++ == 4) return static_cast<uint8_t>(status | 0x40 | index);
        const unsigned tile = read(entry + 2);
        const unsigned attributesByte = read(entry + 3);
        const unsigned color = attributesByte & 15;
        const int x = read(entry + 1) - ((attributesByte & 0x80) ? 32 : 0);
        const unsigned patternRow = spritePatterns + (large ? tile & 0xfc : tile) * 8
            + static_cast<unsigned>(offset) / scale;
        for (unsigned column = 0; column < size; column += 8) {
            const auto pattern = read(patternRow + column * 2);
            for (unsigned dot = 0; dot < 8; ++dot) {
                if ((pattern & (0x80 >> dot)) == 0) continue;
                const int first = x + static_cast<int>((column + dot) * scale);
                for (unsigned repeat = 0; repeat < scale; ++repeat) {
                    const int position = first + static_cast<int>(repeat);
                    auto& pixel = occupied[position + 32];
                    if (pixel & 1) status |= 0x20;
                    pixel |= 1;
                    if (color != 0 && (pixel & 2) == 0 && position >= 0 && position < 256) {
                        output[position] = colors[color];
                        pixel |= 2;
                    }
                }
            }
        }
    }
    return status;
}
} // namespace smsgg
