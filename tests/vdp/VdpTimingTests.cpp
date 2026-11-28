#include "VdpTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Vdp;
using smsgg::VideoStandard;
namespace {
bool vertical() {
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        Vdp vdp(standard);
        const unsigned lines = standard == VideoStandard::Ntsc ? 262 : 313;
        if (vdp.completedFrames() != 0 || vdp.readVCounter() != 0) return false;
        unsigned counter = 0;
        for (unsigned frame = 0; frame < 3; ++frame) {
            for (unsigned line = 0; line < lines; ++line) {
                const uint64_t start = (uint64_t(frame) * lines + line) * 228;
                vdp.advanceTo(start); vdp.advanceTo(start);
                if (vdp.completedFrames() != frame || vdp.readVCounter() != counter) return false;
                vdp.advanceTo(start + 211);
                if (vdp.readVCounter() != counter) return false;
                ++counter;
                if (standard == VideoStandard::Ntsc && line == 218) counter = 0xd5;
                if (standard == VideoStandard::Pal && line == 242) counter = 0xba;
                if (line + 1 == lines) counter = 0;
                for (const unsigned phase : {212u, 213u, 227u}) {
                    vdp.advanceTo(start + phase);
                    if (vdp.readVCounter() != counter || vdp.completedFrames() != frame) return false;
                }
                vdp.advanceTo(start + 228);
                if (vdp.readVCounter() != counter) return false;
            }
        }
        vdp.advanceTo(uint64_t(lines) * 228 * 7 + 229);
        if (vdp.completedFrames() != 7 || vdp.readVCounter() != 1) return false;
    }
    return true;
}
bool horizontal() {
    for (const auto machine : {smsgg::Machine::Sms, smsgg::Machine::GameGear}) {
        for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
            Vdp vdp(standard, machine);
            if (vdp.readHCounter() != 0) return false;
            const uint64_t frame = (standard == VideoStandard::Ntsc ? 262u : 313u) * 228;
            const unsigned repeatedTick = machine == smsgg::Machine::Sms ? 2 : 0;
            uint8_t expected = 0;
            for (const auto base : {uint64_t{0}, frame}) {
                // Independent two-dot tick schedule, not the product's division formula.
                unsigned ticks = 0, nextTick = machine == smsgg::Machine::Sms ? 1 : 2;
                for (unsigned phase = 0; phase < 228; ++phase) {
                    if (phase == nextTick) {
                        ++ticks;
                        nextTick += ticks % 3 == repeatedTick ? 2 : 1;
                    }
                    vdp.advanceTo(base + phase);
                    if (vdp.readHCounter() != expected) return false;
                    vdp.latchHCounter();
                    expected = static_cast<uint8_t>(ticks <= 147 ? ticks : ticks + 85);
                    if (vdp.readHCounter() != expected) return false;
                    vdp.readVCounter();
                    if (vdp.readHCounter() != expected) return false;
                }
            }
            vdp.advanceTo(frame + 228); vdp.latchHCounter();
            if (vdp.readHCounter() != 0) return false;
            vdp.writeControl(0x55); vdp.readVCounter(); vdp.readHCounter();
            vdp.writeControl(0x41); vdp.writeData(0xa5);
            smsgg::test::command(vdp, 0x0155, 0);
            if (vdp.readData() != 0xa5) return false;
        }
    }
    return true;
}
bool frameIrq() {
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        Vdp vdp(standard);
        const uint64_t frame = (standard == VideoStandard::Ntsc ? 262u : 313u) * 228;
        constexpr uint64_t request = 192 * 228 + 212;
        constexpr uint64_t flag = request + 1;
        if (vdp.interruptPending() || vdp.readStatus() != 0x1f) return false;
        smsgg::test::reg(vdp, 1, 0x20); // Frame enable while display remains blank.
        vdp.advanceTo(request - 1);
        if (vdp.interruptPending() || vdp.readStatus() != 0x1f) return false;
        vdp.advanceTo(request);
        if (!vdp.interruptPending()) return false;
        smsgg::test::reg(vdp, 1, 0);
        if (vdp.interruptPending()) return false;
        smsgg::test::reg(vdp, 1, 0x20);
        if (!vdp.interruptPending() || vdp.readStatus() != 0x1f || vdp.interruptPending()) return false;
        vdp.advanceTo(flag);
        if (vdp.interruptPending() || vdp.readStatus() != 0x9f || vdp.interruptPending()
            || vdp.readStatus() != 0x1f) return false;
        vdp.advanceTo(44005);
        if (vdp.interruptPending()) return false;
        smsgg::test::reg(vdp, 1, 0);
        vdp.advanceTo(frame + request);
        if (vdp.interruptPending()) return false;
        vdp.advanceTo(frame * 2);
        smsgg::test::reg(vdp, 1, 0x20);
        if (!vdp.interruptPending() || vdp.readStatus() != 0x9f) return false;
        vdp.advanceTo(frame * 2 + flag);
        if (!vdp.interruptPending() || vdp.readStatus() != 0x9f) return false;
    }
    return true;
}
bool lineIrq() {
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        const unsigned lines = standard == VideoStandard::Ntsc ? 262 : 313;
        const uint64_t frame = uint64_t(lines) * 228;
        for (unsigned reload = 0; reload < 256; ++reload) {
            Vdp vdp(standard);
            smsgg::test::reg(vdp, 0, 0x10); smsgg::test::reg(vdp, 10, static_cast<uint8_t>(reload));
            vdp.advanceTo(frame); vdp.readStatus(); // Vblank has loaded R10 for the next frame.
            for (unsigned f = 1; f <= 3; ++f) {
                for (unsigned line = 0; line < lines; ++line) {
                    const uint64_t end = (uint64_t(f) * lines + line + 1) * 228;
                    vdp.advanceTo(end - 16);
                    if (vdp.interruptPending()) return false;
                    vdp.advanceTo(end - 15);
                    const unsigned nextLine = (line + 1) % lines;
                    const bool event = nextLine <= 192 && (nextLine + 1) % (reload + 1) == 0;
                    if (vdp.interruptPending() != event) {
                        std::cerr << "line IRQ reload=" << reload << " frame=" << f << " line=" << line << '\n'; return false;
                    }
                    if (event) {
                        smsgg::test::reg(vdp, 0, 0);
                        if (vdp.interruptPending()) return false;
                        smsgg::test::reg(vdp, 0, 0x10);
                        if (!vdp.interruptPending()) return false;
                    }
                    if (vdp.readStatus() != (line == 192 ? 0x9f : 0x1f)) return false;
                    vdp.advanceTo(end - 14);
                    if (vdp.interruptPending()) return false;
                    vdp.advanceTo(end);
                    if (vdp.readStatus() != 0x1f || vdp.interruptPending()) return false;
                }
            }
        }
    }
    return true;
}
bool reloadAndSources() {
    Vdp vdp(VideoStandard::Ntsc);
    smsgg::test::reg(vdp, 0, 0x10); smsgg::test::reg(vdp, 10, 2);
    vdp.advanceTo(59736); vdp.readStatus();
    vdp.advanceTo(59736 + 213); // First two active ticks have reduced the counter to zero.
    smsgg::test::reg(vdp, 10, 0);
    vdp.advanceTo(59736 + 228 + 212);
    if (vdp.interruptPending()) return false;
    vdp.advanceTo(59736 + 228 + 213);
    if (!vdp.interruptPending()) return false;
    vdp.readStatus(); smsgg::test::reg(vdp, 10, 5);
    vdp.advanceTo(59736 + 2 * 228 + 213); // Old zero counter expires, then reloads the new five.
    if (!vdp.interruptPending()) return false;
    vdp.readStatus();
    for (unsigned line = 3; line <= 7; ++line) {
        vdp.advanceTo(59736 + line * 228 + 213);
        if (vdp.interruptPending()) return false;
        vdp.readStatus();
    }
    vdp.advanceTo(59736 + 8 * 228 + 213);
    if (!vdp.interruptPending()) return false;
    smsgg::test::reg(vdp, 10, 0xc0);
    vdp.advanceTo(59736 * 2); vdp.readStatus();
    smsgg::test::reg(vdp, 1, 0x20);
    vdp.advanceTo(59736 * 2 + 44004);
    smsgg::test::reg(vdp, 1, 0);
    if (!vdp.interruptPending()) return false; // Line source remains.
    smsgg::test::reg(vdp, 0, 0);
    if (vdp.interruptPending()) return false;
    smsgg::test::reg(vdp, 1, 0x20);
    return vdp.interruptPending() && vdp.readStatus() == 0x9f && !vdp.interruptPending();
}
} // namespace
int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "vertical") return vertical() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "horizontal") return horizontal() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "frame_irq") return frameIrq() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "line_irq") return lineIrq() ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "reload_sources") return reloadAndSources() ? 0 : 1;
    std::cerr << "Failed VDP timing test\n"; return 1;
}
