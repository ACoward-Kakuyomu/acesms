#include "VdpTestSupport.h"

#include <iostream>
#include <memory>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <vector>
#include <type_traits>

using smsgg::Vdp;
using smsgg::VideoStandard;
using smsgg::test::command;
using smsgg::test::reg;
static_assert(std::is_same_v<decltype(std::declval<const Vdp&>().framePixels()), std::span<const smsgg::Rgba8, 256 * 192>>);

namespace {
// Diagnostic timestamp only; the emulated clock remains inside Vdp.
uint64_t observedTstate{};
void advance(Vdp& vdp, uint64_t target) { observedTstate = target; vdp.advanceTo(target); }
bool same(smsgg::Rgba8 a, smsgg::Rgba8 b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
bool solid(Vdp& vdp, smsgg::Rgba8 expected) {
    const auto pixels = vdp.framePixels();
    for (size_t i = 0; i < pixels.size(); ++i) {
        if (!same(pixels[i], expected)) {
            std::cerr << "tstate=" << observedTstate << " status=" << int(vdp.readStatus()) << " frame=" << vdp.completedFrames() << " y=" << i / 256 << " x=" << i % 256
                << " rgba=" << int(pixels[i].r) << ',' << int(pixels[i].g) << ',' << int(pixels[i].b) << ',' << int(pixels[i].a)
                << " expected=" << int(expected.r) << ',' << int(expected.g) << ',' << int(expected.b) << ',' << int(expected.a) << '\n';
            return false;
        }
    }
    return true;
}
bool backdrop() {
    constexpr uint8_t levels[]{0, 85, 170, 255};
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        auto vdp = std::make_unique<Vdp>(standard);
        const uint64_t frame = (standard == VideoStandard::Ntsc ? 262u : 313u) * 228;
        observedTstate = 0; if (!solid(*vdp, {0, 0, 0, 255})) return false;
        for (unsigned color = 0; color < 64; ++color) {
            const uint8_t index = static_cast<uint8_t>(16 + color % 16), raw = static_cast<uint8_t>(color | 0xc0);
            const uint16_t address = static_cast<uint16_t>(index + (color % 2 ? 0x20 : 0));
            command(*vdp, address, 1); vdp->writeData(0xa5);
            command(*vdp, address, 3); vdp->writeData(raw);
            if (vdp->readData() != raw) return false;
            command(*vdp, address, 0);
            if (vdp->readData() != 0xa5) return false; // CRAM writes do not write VRAM.
            reg(*vdp, 7, static_cast<uint8_t>(0xf0 | color % 16));
            advance(*vdp, (color + 1) * frame);
            if (!solid(*vdp, {levels[color % 4], levels[color / 4 % 4], levels[color / 16], 255})) return false;
        }
        for (uint8_t index = 11; index < 16; ++index) {
            reg(*vdp, index, 0);
            advance(*vdp, (64 + index - 10) * frame);
            if (!solid(*vdp, {255, 255, 255, 255})) return false;
        }
        command(*vdp, 0x3fff, 3); vdp->writeData(0x15); vdp->writeData(0x2a);
        if (vdp->readData() != 0x2a) return false;
        reg(*vdp, 7, 15); advance(*vdp, 70 * frame);
        if (!solid(*vdp, {85, 85, 85, 255})) return false;
    }
    return true;
}
using Tile = std::array<std::array<uint8_t, 8>, 8>;
constexpr Tile pattern{{
    {1,2,4,8,3,5,6,7}, {8,4,2,1,7,6,5,3}, {15,14,13,12,11,10,9,0}, {0,9,10,11,12,13,14,15},
    {3,1,4,1,5,9,2,6}, {5,3,5,8,9,7,9,3}, {2,3,8,4,6,2,6,4}, {3,3,8,3,2,7,9,5}}};
void putTile(Vdp& vdp, unsigned index, const Tile& tile) {
    command(vdp, static_cast<uint16_t>(index * 32), 1);
    for (const auto& row : tile) {
        for (unsigned plane = 0; plane < 4; ++plane) {
            uint8_t byte = 0;
            for (unsigned x = 0; x < 8; ++x) if (row[x] / (1u << plane) % 2) byte |= static_cast<uint8_t>(128 >> x);
            vdp.writeData(byte);
        }
    }
}
void setup(Vdp& vdp) {
    reg(vdp, 0, 6); reg(vdp, 1, 0x40); reg(vdp, 2, 0x0e); reg(vdp, 5, 0x7e);
    command(vdp, 0x3f00, 1); vdp.writeData(0xd0);
    command(vdp, 0, 3);
    for (uint8_t i = 0; i < 32; ++i) vdp.writeData(i);
}
smsgg::Rgba8 color(unsigned index) {
    constexpr uint8_t values[]{0, 85, 170, 255};
    return {values[index % 4], values[index / 4 % 4], values[index / 16 % 4], 255};
}
bool pixel(Vdp& vdp, unsigned x, unsigned y, smsgg::Rgba8 expected) {
    const auto actual = vdp.framePixels()[y * 256 + x];
    if (same(actual, expected)) return true;
    std::cerr << "tstate=" << observedTstate << " status=" << int(vdp.readStatus()) << " frame=" << vdp.completedFrames() << " y=" << y << " x=" << x
        << " rgba=" << int(actual.r) << ',' << int(actual.g) << ',' << int(actual.b) << ',' << int(actual.a)
        << " expected=" << int(expected.r) << ',' << int(expected.g) << ',' << int(expected.b) << ',' << int(expected.a) << '\n';
    return false;
}
bool background() {
    for (unsigned base = 0; base < 0x4000; base += 0x800) {
        auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
        const unsigned indexes[]{base == 0 ? 128u : 0u, 255, base == 0x2000 ? 384u : 256u, 511};
        for (unsigned ordinal = 0; ordinal < 4; ++ordinal) {
            auto tile = pattern;
            for (auto& row : tile) for (auto& p : row) p = static_cast<uint8_t>((p + ordinal * 3) % 16);
            putTile(*vdp, indexes[ordinal], tile);
        }
        reg(*vdp, 2, static_cast<uint8_t>(0xf1 | (base / 0x800 * 2)));
        command(*vdp, static_cast<uint16_t>(base), 1);
        for (unsigned entry = 0; entry < 32 * 28; ++entry) {
            const unsigned attribute = (entry / 4 % 128) * 512;
            const auto word = indexes[entry % 4] + attribute;
            vdp->writeData(static_cast<uint8_t>(word)); vdp->writeData(static_cast<uint8_t>(word >> 8));
        }
        // At base 3800 the lower, invisible table rows contain SAT. Keep the terminator intact.
        command(*vdp, 0x3f00, 1); vdp->writeData(0xd0);
        advance(*vdp, 59736);
        for (unsigned y = 0; y < 192; ++y) {
            for (unsigned x = 0; x < 256; ++x) {
                const unsigned entry = y / 8 * 32 + x / 8, a = entry / 4 % 128;
                const unsigned px = a % 2 ? 7 - x % 8 : x % 8, py = a / 2 % 2 ? 7 - y % 8 : y % 8;
                const unsigned p = (pattern[py][px] + entry % 4 * 3) % 16;
                if (!pixel(*vdp, x, y, color(p + (a / 4 % 2) * 16))) return false;
            }
        }
    }
    return true;
}
bool modes() {
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
    Tile tile{}; for (auto& row : tile) row.fill(1); putTile(*vdp, 1, tile);
    command(*vdp, 0x3800, 1);
    for (unsigned i = 0; i < 768; ++i) { vdp->writeData(1); vdp->writeData(0); }
    command(*vdp, 31, 3); vdp->writeData(0x3f); reg(*vdp, 7, 15);
    constexpr bool supported[4][4]{{false,false,false,false}, {false,false,false,false},
        {true,true,false,false}, {true,false,false,true}};
    uint64_t time = 0;
    for (unsigned r0 = 0; r0 < 4; ++r0) for (unsigned r1 = 0; r1 < 4; ++r1) for (const uint8_t display : {0, 0x40}) {
        reg(*vdp, 0, static_cast<uint8_t>(r0 * 2)); reg(*vdp, 1, static_cast<uint8_t>(r1 * 8 + display));
        advance(*vdp, time += 59736);
        if (!solid(*vdp, supported[r0][r1] && display ? color(1) : color(63))) return false;
    }
    return true;
}
void scrollingScene(Vdp& vdp) {
    setup(vdp); putTile(vdp, 1, pattern);
    auto other = pattern; for (auto& row : other) for (auto& p : row) p = static_cast<uint8_t>((p + 3) % 16);
    putTile(vdp, 2, other); reg(vdp, 7, 15);
    command(vdp, 0x3800, 1);
    for (unsigned row = 0; row < 28; ++row) for (unsigned column = 0; column < 32; ++column) {
        vdp.writeData(static_cast<uint8_t>(1 + (row + column) % 2)); vdp.writeData(row % 2 ? 8 : 0);
    }
}
smsgg::Rgba8 scrolledPixel(unsigned x, unsigned y, unsigned h, unsigned v, unsigned flags) {
    if ((flags / 32 % 2) && x < 8) return color(31);
    if ((flags / 64 % 2) && y < 16) h = 0;
    if (x < h % 8) return color(31);
    const auto sx = (x + 256 - h) % 256;
    const auto sy = (y + ((flags / 128 % 2) && x >= 192 ? 0 : v % 224)) % 224;
    const auto p = (pattern[sy % 8][sx % 8] + (sx / 8 + sy / 8) % 2 * 3) % 16;
    return color(p + sy / 8 % 2 * 16);
}
bool scroll() {
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); scrollingScene(*vdp);
    uint64_t time = 0;
    for (const uint8_t h : {0,1,7,8,255}) for (const uint8_t v : {0,1,7,8,223,224,255}) {
        for (const uint8_t flags : {0,0x20,0x40,0x80,0xc0,0xe0}) {
            reg(*vdp, 0, static_cast<uint8_t>(6 | flags)); reg(*vdp, 8, h); reg(*vdp, 9, v);
            advance(*vdp, time += 59736 * 2); // R9 becomes effective at the intervening wrap.
            for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
                if (!pixel(*vdp, x, y, scrolledPixel(x, y, h, v, flags))) return false;
            }
        }
    }
    // Fixed boundary vectors independent of the scroll expectation function.
    reg(*vdp, 0, 6); reg(*vdp, 8, 1); reg(*vdp, 9, 0); advance(*vdp, time += 59736 * 2);
    if (!pixel(*vdp, 0, 0, color(31)) || !pixel(*vdp, 1, 0, color(1))) return false;
    reg(*vdp, 8, 8); advance(*vdp, time += 59736 * 2);
    return pixel(*vdp, 0, 0, color(4)) && pixel(*vdp, 8, 0, color(1));
}
bool scrollLatches() {
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); scrollingScene(*vdp);
    advance(*vdp, 96 * 228 + 100);
    reg(*vdp, 8, 8); reg(*vdp, 9, 8);
    advance(*vdp, 59736);
    for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
        if (!pixel(*vdp, x, y, scrolledPixel(x, y, y <= 96 ? 0 : 8, 0, 0))) return false;
    }
    advance(*vdp, 59736 * 2);
    for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
        if (!pixel(*vdp, x, y, scrolledPixel(x, y, 8, 8, 0))) return false;
    }
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        const unsigned frame = (standard == VideoStandard::Ntsc ? 262u : 313u) * 228;
        for (const unsigned phase : {210u, 211u, 212u}) {
            auto original = std::make_unique<Vdp>(standard); scrollingScene(*original);
            original->advanceTo(96 * 228);
            original->writeControl(8, 96 * 228 + 200);
            original->writeControl(0x88, 96 * 228 + phase);
            original->advanceTo(96 * 228 + 212);
            auto saved = std::make_unique<smsgg::VdpState>();
            original->captureState(*saved);
            auto restored = std::make_unique<Vdp>(standard);
            restored->restoreState(*saved);
            for (auto* system : {original.get(), restored.get()}) {
                advance(*system, frame);
                for (unsigned y = 96; y <= 98; ++y) {
                    const unsigned h = (y == 98 || (y == 97 && phase == 210)) ? 8 : 0;
                    for (unsigned x = 0; x < 256; ++x)
                        if (!pixel(*system, x, y, scrolledPixel(x, y, h, 0, 0))) return false;
                }
            }
        }
        auto wrapped = std::make_unique<Vdp>(standard); scrollingScene(*wrapped);
        wrapped->advanceTo(frame - 228);
        wrapped->writeControl(8, frame - 18);
        wrapped->writeControl(0x88, frame - 18); // Last write before the latch for row 0.
        wrapped->writeControl(0, frame - 17);
        wrapped->writeControl(0x88, frame - 17);
        advance(*wrapped, frame * 2);
        for (unsigned x = 0; x < 256; ++x) {
            if (!pixel(*wrapped, x, 0, scrolledPixel(x, 0, 8, 0, 0)) ||
                !pixel(*wrapped, x, 1, scrolledPixel(x, 1, 0, 0, 0))) return false;
        }
    }
    return true;
}
void putSprite(Vdp& vdp, unsigned base, unsigned index, uint8_t y, uint8_t x, uint8_t tile) {
    command(vdp, static_cast<uint16_t>(base + index), 1); vdp.writeData(y);
    command(vdp, static_cast<uint16_t>(base + 0x80 + index * 2), 1); vdp.writeData(x); vdp.writeData(tile);
}
bool spriteGeometry() {
    for (const unsigned base : {0x1000,0x3f00}) for (const unsigned bank : {0,1}) {
        auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
        reg(*vdp, 5, static_cast<uint8_t>(0x81 | base / 128)); reg(*vdp, 6, static_cast<uint8_t>(0xfb | bank * 4));
        for (unsigned i = 0; i < 4; ++i) {
            auto tile = pattern;
            for (auto& row : tile) for (auto& p : row) p = static_cast<uint8_t>((p + (i % 2) * 3 + (i / 2) * 6) % 16);
            putTile(*vdp, i / 2 * 256 + 2 + i % 2, tile);
        }
        command(*vdp, static_cast<uint16_t>(base + 1), 1); vdp->writeData(0xd0);
        uint64_t time = 0;
        for (const unsigned large : {0,1}) for (const unsigned zoom : {1,2}) for (const unsigned shift : {0,8}) {
            reg(*vdp, 0, static_cast<uint8_t>(6 + shift)); reg(*vdp, 1, static_cast<uint8_t>(0x40 + large * 2 + zoom - 1));
            for (const uint8_t x : {0,7,248,255}) for (const uint8_t y : {0xff,0,0xcf,0xd0,0xe0}) {
                putSprite(*vdp, base, 0, y, x, 3); advance(*vdp, time += 59736);
                for (unsigned screenY = 0; screenY < 192; ++screenY) for (unsigned screenX = 0; screenX < 256; ++screenX) {
                    auto expected = color(0);
                    const unsigned dy = (screenY + 256 - (unsigned(y) + 1) % 256) % 256;
                    const int dx = int(screenX) - (int(x) - int(shift));
                    if (y != 0xd0 && dy < (large ? 16u : 8u) * zoom && dx >= 0 && dx < int(8 * zoom)) {
                        const unsigned row = dy / zoom;
                        const auto value = (pattern[row % 8][dx / zoom] + bank * 6 + (large ? (row >= 8 ? 3 : 0) : 3)) % 16;
                        if (value) expected = color(16 + value);
                    }
                    if (!pixel(*vdp, screenX, screenY, expected)) return false;
                }
            }
        }
    }
    return true;
}
bool priority() {
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp); putTile(*vdp, 1, pattern);
    for (unsigned index = 2; index <= 3; ++index) {
        auto tile = pattern;
        for (auto& row : tile) for (auto& p : row) p = static_cast<uint8_t>((p + (index == 2 ? 3 : 9)) % 16);
        putTile(*vdp, index, tile);
    }
    putSprite(*vdp, 0x3f00, 0, 15, 16, 2); putSprite(*vdp, 0x3f00, 1, 15, 18, 3);
    command(*vdp, 0x3f02, 1); vdp->writeData(0xd0);
    putSprite(*vdp, 0x3f00, 3, 15, 32, 3); // Must stay hidden after the terminator.
    uint64_t time = 0;
    for (const unsigned palette : {0,1}) for (const unsigned priorityBit : {0,1}) {
        command(*vdp, 0x3800, 1);
        for (unsigned i = 0; i < 768; ++i) { vdp->writeData(1); vdp->writeData(static_cast<uint8_t>(palette * 8 + priorityBit * 16)); }
        advance(*vdp, time += 59736);
        for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
            const unsigned bg = pattern[y % 8][x % 8];
            auto expected = color(bg + palette * 16);
            unsigned sprite = 0;
            if (y >= 16 && y < 24) {
                if (x >= 16 && x < 24) sprite = (pattern[y - 16][x - 16] + 3) % 16;
                if (!sprite && x >= 18 && x < 26) sprite = (pattern[y - 16][x - 18] + 9) % 16;
            }
            if (sprite && !(priorityBit && bg)) expected = color(16 + sprite);
            if (!pixel(*vdp, x, y, expected)) return false;
        }
    }
    return true;
}
bool overflow() {
    for (const unsigned count : {0,1,8,9,64}) for (const unsigned kind : {0,1,2}) {
        auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
        Tile tile{}; for (auto& row : tile) row.fill(1); putTile(*vdp, 1, tile);
        reg(*vdp, 0, static_cast<uint8_t>(6 + (kind == 2 ? 8 : 0)));
        for (unsigned i = 0; i < count; ++i) putSprite(*vdp, 0x3f00, i, 0,
            static_cast<uint8_t>(kind == 2 ? 0 : i * 8), static_cast<uint8_t>(kind == 1 ? 0 : 1));
        if (count < 64) { command(*vdp, static_cast<uint16_t>(0x3f00 + count), 1); vdp->writeData(0xd0); }
        advance(*vdp, 227);
        if (vdp->readStatus() != 0x1f) return false;
        advance(*vdp, 228);
        vdp->readData(); vdp->writeData(0);
        const uint8_t expected = count > 8 ? 0x5f : 0x1f;
        if (vdp->readStatus() != expected || vdp->readStatus() != 0x1f) {
            std::cerr << "overflow count=" << count << " kind=" << kind << '\n'; return false;
        }
        advance(*vdp, 455);
        if (vdp->readStatus() != 0x1f) return false;
        advance(*vdp, 456);
        if (vdp->readStatus() != expected) return false;
        reg(*vdp, 1, 0);
        advance(*vdp, 684);
        if (vdp->readStatus() != expected) return false;
        advance(*vdp, 20 * 228);
        if (vdp->readStatus() != expected) return false; // Blank does not clear an existing flag.
        advance(*vdp, 21 * 228);
        if (vdp->readStatus() != 0x1f) return false;
    }
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        auto vdp = std::make_unique<Vdp>(standard); setup(*vdp);
        const uint64_t frame = (standard == VideoStandard::Ntsc ? 262u : 313u) * 228;
        const auto setY = [&](uint8_t y) {
            command(*vdp, 0x3f00, 1);
            for (int i = 0; i < 9; ++i) vdp->writeData(y);
            vdp->writeData(0xd0);
        };
        setY(190);
        if ((vdp->readStatus(191 * 228 - 1) & 0x40) != 0 || (vdp->readStatus(191 * 228) & 0x40) == 0
            || (vdp->readStatus(192 * 228) & 0x40) != 0) return false;
        setY(0xff);
        if ((vdp->readStatus(frame - 1) & 0x40) != 0) return false;
        auto saved = std::make_unique<smsgg::VdpState>();
        auto restored = std::make_unique<Vdp>(standard);
        vdp->captureState(*saved);
        restored->restoreState(*saved);
        if ((vdp->readStatus(frame) & 0x40) == 0 || (restored->readStatus(frame) & 0x40) == 0) return false;
        vdp->captureState(*saved);
        restored->restoreState(*saved);
        if ((restored->readStatus(frame + 1) & 0x40) != 0 || (restored->readStatus(frame + 228) & 0x40) == 0) return false;
    }
    return true;
}
bool collision() {
    for (const unsigned mask : {0,0x20}) for (const unsigned shift : {0,8}) for (const unsigned zoom : {1,2}) {
        for (const uint8_t x : {0,7,8,16,255}) for (const bool disjoint : {false,true}) {
            auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
            Tile first{}, second{};
            for (auto& row : first) row[0] = 1;
            for (auto& row : second) row[disjoint ? 1 : 0] = 2;
            putTile(*vdp, 1, first); putTile(*vdp, 2, second);
            command(*vdp, 17, 3); vdp->writeData(0); vdp->writeData(0); // Both opaque colors are black.
            reg(*vdp, 0, static_cast<uint8_t>(6 + mask + shift)); reg(*vdp, 1, static_cast<uint8_t>(0x40 + zoom - 1));
            putSprite(*vdp, 0x3f00, 0, 0, x, 1); putSprite(*vdp, 0x3f00, 1, 0, x, 2);
            command(*vdp, 0x3f02, 1); vdp->writeData(0xd0);
            advance(*vdp, 455);
            if (vdp->readStatus() != 0x1f) return false;
            advance(*vdp, 456);
            const int left = int(x) - int(shift);
            const bool hit = !disjoint && left + int(zoom) > (mask ? 8 : 0) && left < 256;
            const uint8_t expected = hit ? 0x3f : 0x1f;
            if (vdp->readStatus() != expected || vdp->readStatus() != 0x1f) {
                std::cerr << "collision x=" << int(x) << " mask=" << mask << " shift=" << shift << " zoom=" << zoom << '\n'; return false;
            }
            advance(*vdp, 684);
            if (vdp->readStatus() != expected) return false;
        }
    }
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
    Tile tile{}; for (auto& row : tile) row.fill(1); putTile(*vdp, 1, tile);
    for (unsigned i = 0; i < 9; ++i) putSprite(*vdp, 0x3f00, i, 0, 16, i < 7 ? 0 : 1);
    command(*vdp, 0x3f09, 1); vdp->writeData(0xd0);
    advance(*vdp, 456);
    if (vdp->readStatus() != 0x5f) return false; // The ninth sprite never enters collision detection.
    putSprite(*vdp, 0x3f00, 0, 0, 16, 1); putSprite(*vdp, 0x3f00, 1, 0, 16, 1);
    command(*vdp, 0x3f02, 1); vdp->writeData(0xd0);
    command(*vdp, 0x3800, 1);
    for (unsigned i = 0; i < 768; ++i) { vdp->writeData(1); vdp->writeData(0x10); }
    advance(*vdp, 684);
    if (vdp->readStatus() != 0x3f) return false; // Background priority hides pixels but not collision.
    reg(*vdp, 1, 0); advance(*vdp, 912);
    return vdp->readStatus() == 0x1f;
}
smsgg::Rgba8 fixturePixel(unsigned x, unsigned y, unsigned backdrop) {
    auto expected = x < 8 ? color(backdrop) : scrolledPixel(x, y, 8, 8, 0xe0);
    if (x >= 64 && x < 72 && y >= 48 && y < 56 && x - 64 <= y - 48) return color(28);
    if (x >= 68 && x < 76 && y >= 50 && y < 58 && x - 68 == y - 50) return color(30);
    return expected;
}
bool frameLifecycle(const std::filesystem::path& folder) {
    std::error_code error; std::filesystem::create_directories(folder, error);
    if (error) return false;
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        auto vdp = std::make_unique<Vdp>(standard); scrollingScene(*vdp);
        reg(*vdp, 0, 0xe6); reg(*vdp, 8, 8); reg(*vdp, 9, 8);
        Tile triangle{}, diagonal{};
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            triangle[y][x] = x <= y ? 12 : 0; diagonal[y][x] = x == y ? 14 : 0;
        }
        putTile(*vdp, 3, triangle); putTile(*vdp, 4, diagonal);
        putSprite(*vdp, 0x3f00, 0, 47, 64, 3); putSprite(*vdp, 0x3f00, 1, 49, 68, 4);
        command(*vdp, 0x3f02, 1); vdp->writeData(0xd0);
        const uint64_t frame = (standard == VideoStandard::Ntsc ? 262u : 313u) * 228;
        advance(*vdp, frame - 1);
        if (vdp->completedFrames() != 0 || !solid(*vdp, {0,0,0,255})) return false;
        advance(*vdp, frame); advance(*vdp, frame * 2);
        if (vdp->completedFrames() != 2) return false;
        for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
            if (!pixel(*vdp, x, y, fixturePixel(x, y, 31))) return false;
        }
        const std::vector<smsgg::Rgba8> published(vdp->framePixels().begin(), vdp->framePixels().end());
        advance(*vdp, frame * 2 + 100 * 228);
        reg(*vdp, 7, 14);
        for (size_t i = 0; i < published.size(); ++i) if (!same(published[i], vdp->framePixels()[i])) return false;
        advance(*vdp, frame * 3);
        if (vdp->completedFrames() != 3 || !pixel(*vdp, 0, 99, color(31)) || !pixel(*vdp, 0, 100, color(30))) return false;
        advance(*vdp, frame * 6 + 229);
        if (vdp->completedFrames() != 6 || vdp->readVCounter() != 1) return false;
        for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
            if (!pixel(*vdp, x, y, fixturePixel(x, y, 30))) {
                std::cerr << "tstate=" << frame * 6 + 229 << " status=" << int(vdp->readStatus()) << '\n'; return false;
            }
        }
        const auto path = folder / (standard == VideoStandard::Ntsc ? "fixture-ntsc.ppm" : "fixture-pal.ppm");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "P6\n256 192\n255\n";
        for (const auto rgba : vdp->framePixels()) {
            const char rgb[]{static_cast<char>(rgba.r), static_cast<char>(rgba.g), static_cast<char>(rgba.b)};
            output.write(rgb, 3);
        }
        output.close(); if (!output) return false;
    }
    return true;
}
bool cramProtocol() {
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
    command(*vdp, 0x3fff, 3); vdp->writeData(0x15); vdp->writeData(0xea);
    if (vdp->readData() != 0xea || vdp->readData() != 0) return false;
    vdp->writeData(6); // Reads left code 11 selected; address is now 0003.
    command(*vdp, 3, 0);
    if (vdp->readData() != 0) return false;
    advance(*vdp, 59736);
    if (!solid(*vdp, color(0x2a))) return false; // CRAM[0] received the wrap from 3FFF.
    Tile tile{}; for (auto& row : tile) row.fill(3); putTile(*vdp, 1, tile);
    command(*vdp, 0x3800, 1);
    for (unsigned i = 0; i < 768; ++i) { vdp->writeData(1); vdp->writeData(0); }
    advance(*vdp, 59736 * 2);
    if (!solid(*vdp, color(6))) return false;
    reg(*vdp, 1, 0); reg(*vdp, 7, 15); advance(*vdp, 59736 * 3);
    return solid(*vdp, color(0x15));
}
bool combinedStatus() {
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc); setup(*vdp);
    Tile tile{}; for (auto& row : tile) row.fill(1); putTile(*vdp, 1, tile);
    for (unsigned i = 0; i < 9; ++i) putSprite(*vdp, 0x3f00, i, 0, 16, 1);
    command(*vdp, 0x3f09, 1); vdp->writeData(0xd0);
    reg(*vdp, 0, 0x16); reg(*vdp, 1, 0x60); reg(*vdp, 10, 0);
    advance(*vdp, 59736); vdp->readStatus();
    advance(*vdp, 59736 + 44004);
    if (!vdp->interruptPending() || vdp->readStatus() != 0xff || vdp->interruptPending() || vdp->readStatus() != 0x1f) return false;
    reg(*vdp, 0, 0); // Unsupported mode must not generate new sprite flags.
    advance(*vdp, 59736 * 2 + 456);
    return vdp->readStatus() == 0x1f;
}
} // namespace
int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "frames") return frameLifecycle(argv[2]) ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "backdrop") return backdrop() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "background") return background() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "modes") return modes() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "scroll") return scroll() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "scroll_latches") return scrollLatches() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "sprite_geometry") return spriteGeometry() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "priority") return priority() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "overflow") return overflow() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "collision") return collision() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "cram_protocol") return cramProtocol() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "combined_status") return combinedStatus() ? 0 : 1;
    std::cerr << "Failed Mode 4 renderer test\n"; return 1;
}
