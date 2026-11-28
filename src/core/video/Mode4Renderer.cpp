#include "core/video/Mode4Renderer.h"

#include <algorithm>

namespace smsgg {
namespace {
bool isMode192(std::span<const uint8_t, 11> registers) {
    const unsigned mode = (registers[1] >> 3) & 3;
    return (registers[0] & 4) != 0
        && ((registers[0] & 2) != 0 ? mode == 0 || mode == 3 : (registers[1] & 0x10) == 0);
}

template<class Visit>
bool visitSprites(std::span<const uint8_t, 0x4000> vram, std::span<const uint8_t, 11> registers,
    unsigned line, Visit visit) {
    const unsigned sat = (registers[5] & 0x7e) << 7;
    const unsigned zoom = (registers[1] & 1) != 0 ? 2 : 1;
    const unsigned height = ((registers[1] & 2) != 0 ? 16u : 8u) * zoom;
    unsigned selected = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const unsigned y = vram[sat + i];
        if (y == 0xd0) break;
        const unsigned dy = (line - ((y + 1) & 255)) & 255;
        if (dy >= height) continue;
        if (selected++ == 8) return true;
        visit(sat + 0x80 + i * 2, dy / zoom, zoom);
    }
    return false;
}

unsigned patternPixel(std::span<const uint8_t, 0x4000> vram, unsigned index, unsigned row, unsigned column) {
    const unsigned address = index * 32 + row * 4;
    unsigned pixel = 0;
    for (unsigned plane = 0; plane < 4; ++plane) pixel |= ((vram[address + plane] >> (7 - column)) & 1) << plane;
    return pixel;
}
template<class Pixel>
uint8_t composeLine(std::span<const uint8_t, 0x4000> vram, std::span<const Pixel, 32> palette,
    std::span<const uint8_t, 11> registers, unsigned scrollX, uint8_t scrollY, unsigned line, std::span<Pixel, 256> output) {
    const auto backdrop = palette[16 + (registers[7] & 15)];
    if (!isMode192(registers) || (registers[1] & 0x40) == 0) {
        std::fill(output.begin(), output.end(), backdrop);
        return 0;
    }
    const unsigned base = (registers[2] & 0x0e) << 10;
    if ((registers[0] & 0x40) != 0 && line < 16) scrollX = 0;
    std::array<bool, 256> backgroundPriority{};
    for (unsigned x = 0; x < 256; ++x) {
        if (x < (scrollX & 7) || ((registers[0] & 0x20) != 0 && x < 8)) { output[x] = backdrop; continue; }
        const unsigned sx = (x + 256 - scrollX) & 255;
        const unsigned sy = (line + ((registers[0] & 0x80) != 0 && x >= 192 ? 0 : scrollY % 224)) % 224;
        const unsigned entry = (base + (sy / 8 * 32 + sx / 8) * 2) & 0x3fff;
        const unsigned word = vram[entry] | (vram[(entry + 1) & 0x3fff] << 8);
        const unsigned row = (word & 0x400) != 0 ? 7 - sy % 8 : sy % 8;
        const unsigned column = (word & 0x200) != 0 ? 7 - sx % 8 : sx % 8;
        const unsigned pixel = patternPixel(vram, word & 0x1ff, row, column);
        backgroundPriority[x] = (word & 0x1000) != 0 && pixel != 0;
        output[x] = palette[((word & 0x800) != 0 ? 16 : 0) + pixel];
    }
    const bool large = (registers[1] & 2) != 0;
    uint8_t flags = 0;
    std::array<bool, 256> spriteSeen{};
    const bool overflow = visitSprites(vram, registers, line, [&](unsigned attributes, unsigned row, unsigned zoom) {
        const int left = int(vram[attributes]) - ((registers[0] & 8) != 0 ? 8 : 0);
        const unsigned index = (large ? vram[attributes + 1] & 0xfe : vram[attributes + 1])
            + ((registers[6] & 4) != 0 ? 256 : 0) + row / 8;
        for (unsigned offset = 0; offset < 8 * zoom; ++offset) {
            const int x = left + int(offset);
            if (x < 0 || x >= 256 || ((registers[0] & 0x20) != 0 && x < 8)) continue;
            const auto pixel = patternPixel(vram, index, row % 8, offset / zoom);
            if (pixel != 0) {
                if (spriteSeen[x]) flags |= 0x20;
                else {
                    if (!backgroundPriority[x]) output[x] = palette[16 + pixel];
                    spriteSeen[x] = true;
                }
            }
        }
    });
    return flags | (overflow ? 0x40 : 0);
}
} // namespace

bool Mode4Renderer::spriteOverflow(std::span<const uint8_t, 0x4000> vram, std::span<const uint8_t, 11> registers,
    unsigned line) {
    return isMode192(registers) && visitSprites(vram, registers, line, [](unsigned, unsigned, unsigned) {});
}

uint8_t Mode4Renderer::renderLine(std::span<const uint8_t, 0x4000> vram, std::span<const Rgba8, 32> palette,
    std::span<const uint8_t, 11> registers, uint8_t scrollX, uint8_t scrollY, unsigned line, std::span<Rgba8, 256> output,
    std::span<const CramChange> changes) {
    if (changes.empty()) return composeLine(vram, palette, registers, scrollX, scrollY, line, output);

    constexpr auto identity = [] {
        std::array<uint8_t, 32> entries{};
        for (uint8_t i = 0; i < entries.size(); ++i) entries[i] = i;
        return entries;
    }();
    std::array<uint8_t, 256> indices;
    const auto flags = composeLine<uint8_t>(vram, identity, registers, scrollX, scrollY, line, indices);

    // Resolve each span with the palette visible there, rolling changes back from the right edge.
    std::array<Rgba8, 32> colors;
    std::copy(palette.begin(), palette.end(), colors.begin());
    unsigned right = 256;
    const auto paintTo = [&](unsigned left) {
        while (right > left) {
            --right;
            output[right] = colors[indices[right]];
        }
    };
    for (auto change = changes.rbegin(); change != changes.rend(); ++change) {
        paintTo(change->dot);
        colors[change->index] = change->previous;
    }
    paintTo(0);
    return flags;
}
} // namespace smsgg
