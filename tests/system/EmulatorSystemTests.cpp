#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "Stage5TestRom.h"
#include "Sg1000TestRom.h"
#include "../cpu/Z80TestSupport.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>
#include <type_traits>

using smsgg::EmulatorSystem;
using smsgg::VideoStandard;
namespace {
smsgg::RomImage image(std::vector<uint8_t> bytes, smsgg::MapperKind mapper = smsgg::MapperKind::RomOnly) {
    return std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::move(bytes), {mapper}));
}
uint32_t step(EmulatorSystem& system, smsgg::PadState pad = {}, bool pause = false) {
    return std::get<uint32_t>(system.step(pad, pause));
}
bool clockProgression() {
    static_assert(!std::is_copy_constructible_v<EmulatorSystem>);
    static_assert(!std::is_move_constructible_v<EmulatorSystem>);
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        std::vector<uint8_t> bytes(8192);
        bytes[0] = 0xc3; // JP 0000, exactly 10 T per step.
        auto system = std::make_unique<EmulatorSystem>(image(bytes), standard, 48000);
        if (!smsgg::test::sameRegisters(system->cpu().registers(), {.sp = 0xdff8})
            || system->cpu().iff1() || system->cpu().iff2() || system->cpu().interruptMode()
            || system->tstates() || system->completedFrames()) return false;
        for (unsigned a = 0xc000; a < 0xe000; ++a) if (system->readMemory(static_cast<uint16_t>(a))) return false;
        const uint64_t frameTime = (standard == VideoStandard::Ntsc ? 262 : 313) * 228;
        for (uint64_t frame = 1; frame <= 2; ++frame) {
            while (system->completedFrames() < frame) {
                const auto before = system->tstates();
                if (step(*system) != 10 || system->tstates() != before + 10) return false;
            }
            if (system->tstates() != ((frameTime * frame + 9) / 10) * 10) return false;
            for (const auto pixel : system->framePixels())
                if (pixel.r || pixel.g || pixel.b || pixel.a != 255) return false;
        }
    }
    return true;
}

bool smsBootVdp() {
    std::vector<uint8_t> bytes(8192);
    size_t at = 0;
    auto out = [&](uint8_t port, uint8_t value) {
        for (const uint8_t byte : {uint8_t{0x3e}, value, uint8_t{0xd3}, port}) bytes[at++] = byte;
    };
    auto address = [&](uint16_t value, uint8_t code) {
        out(0xbf, static_cast<uint8_t>(value));
        out(0xbf, static_cast<uint8_t>((code << 6) | (value >> 8)));
    };
    address(1, 3); out(0xbe, 0x03); // Red background color 1.
    address(17, 3); out(0xbe, 0x30); // Blue sprite color 1.
    address(32, 1);
    for (unsigned row = 0; row < 8; ++row) {
        out(0xbe, 0xff); out(0xbe, 0); out(0xbe, 0); out(0xbe, 0);
    }
    address(0x3802, 1); out(0xbe, 1); out(0xbe, 0);
    address(0x3f00, 1); out(0xbe, 0xff); out(0xbe, 0xd0);
    address(0x3f80, 1); out(0xbe, 16); out(0xbe, 1);
    out(0xbf, 0x40); out(0xbf, 0x81); // Enable display without configuring R0, R2 or R5.
    bytes[at] = 0x76;
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        auto system = std::make_unique<EmulatorSystem>(image(bytes), standard, 48000);
        while (system->completedFrames() < 2) step(*system);
        const auto pixels = system->framePixels();
        if (pixels[8].r != 255 || pixels[8].g || pixels[8].b ||
            pixels[16].r || pixels[16].g || pixels[16].b != 255) {
            std::cerr << "SMS startup background/sprite pixels missing\n";
            return false;
        }
    }
    return true;
}

bool interrupts() {
    auto system = std::make_unique<EmulatorSystem>(image(smsgg::test::stage5InputRom()), VideoStandard::Ntsc, 48000);
    // Stop on the first boundary with frame IRQ pending, before the next step can accept it.
    while (system->tstates() < 193 * 228 - 16) step(*system);
    if (!system->cpu().halted() || !system->cpu().canAcceptMaskableInterrupt()) return false;
    const auto pc = system->cpu().registers().pc;
    const auto before = system->tstates();
    if (step(*system, {}, true) != 11 || system->tstates() != before + 11
        || system->cpu().registers().pc != 0x66 || system->cpu().registers().sp != 0xdff6
        || system->cpu().iff1() || !system->cpu().iff2() || system->cpu().halted()
        || system->readMemory(0xdff6) != static_cast<uint8_t>(pc)
        || system->readMemory(0xdff7) != static_cast<uint8_t>(pc >> 8)) return false;
    for (unsigned count = 0; system->cpu().registers().pc != pc && count < 20; ++count) step(*system);
    if (system->cpu().registers().pc != pc || system->readMemory(0xc001) != 1
        || system->readMemory(0xc000) || !system->cpu().iff1()) return false;
    const auto irqTime = system->tstates();
    if (step(*system) != 13 || system->tstates() != irqTime + 13
        || system->cpu().registers().pc != 0x38 || system->cpu().iff1()) return false;
    for (unsigned count = 0; system->cpu().registers().pc != pc && count < 20; ++count) step(*system);
    if (system->cpu().registers().pc != pc || system->readMemory(0xc000) != 1
        || (system->readMemory(0xc003) & 0x80) == 0 || !system->cpu().iff1()) return false;
    // Status read cleared pending; the next step is the main-loop IN, not another IRQ.
    return step(*system) == 11 && system->cpu().registers().pc == pc + 2;
}

bool interruptModes() {
    for (const uint8_t mode : {0x46, 0x56, 0x5e}) for (const bool enable : {false, true}) {
        std::vector<uint8_t> bytes(8192);
        size_t at = 0;
        auto emit = [&](std::initializer_list<uint8_t> values) { for (auto value : values) bytes[at++] = value; };
        emit({0xc3, 0x00, 0x01});
        at = 0x100;
        emit({0xf3, 0x31, 0xf8, 0xdf, 0xed, mode, 0x3e, 0xc0, 0xed, 0x47,
            0x3e, 0x38, 0x32, 0xff, 0xc0, 0xaf, 0x32, 0x00, 0xc1,
            0x3e, 0x20, 0xd3, 0xbf, 0x3e, 0x81, 0xd3, 0xbf,
            0xdb, 0x7e, 0xfe, 0xc1, 0x20, 0xfa}); // Wait until V counter=193 with DI.
        const auto eiPc = at;
        emit({static_cast<uint8_t>(enable ? 0xfb : 0xf3), 0x76});
        auto system = std::make_unique<EmulatorSystem>(image(std::move(bytes)), VideoStandard::Ntsc, 48000);
        unsigned count = 0;
        while (system->cpu().registers().pc != eiPc && count++ < 20000) step(*system);
        if (system->cpu().registers().pc != eiPc || system->tstates() < 193 * 228 - 16
            || system->cpu().iff1() || step(*system) != 4
            || system->cpu().canAcceptMaskableInterrupt()) return false;
        if (step(*system) != 4 || !system->cpu().halted()) return false;
        const auto returnPc = system->cpu().registers().pc;
        const auto time = system->tstates();
        if (enable) {
            const auto expected = mode == 0x5e ? 19u : 13u;
            if (step(*system) != expected || system->tstates() != time + expected
                || system->cpu().registers().pc != 0x38 || system->cpu().iff1()
                || system->readMemory(0xdff6) != static_cast<uint8_t>(returnPc)
                || system->readMemory(0xdff7) != static_cast<uint8_t>(returnPc >> 8)) return false;
        } else {
            if (step(*system) != 4 || system->cpu().registers().pc != returnPc
                || step(*system, {}, true) != 11 || system->cpu().registers().pc != 0x66) return false;
        }
    }
    return true;
}

bool rom(const std::filesystem::path& output) {
    const auto bytes = smsgg::test::stage5InputRom();
    if (!output.empty()) {
        std::filesystem::create_directories(output.parent_path());
        std::ofstream stream(output, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!stream) return false;
    }
    for (const auto standard : {VideoStandard::Ntsc, VideoStandard::Pal}) {
        auto system = std::make_unique<EmulatorSystem>(image(bytes), standard, 48000);
        struct Input { smsgg::PadState pad; uint8_t dc; };
        const Input inputs[]{ {{}, 0xff}, {{.up = true}, 0xfe}, {{.down = true}, 0xfd},
            {{.left = true}, 0xfb}, {{.right = true}, 0xf7}, {{.button1 = true}, 0xef},
            {{.button2 = true}, 0xdf}, {{}, 0xff} };
        for (const auto input : inputs) {
            const bool pressed = input.dc != 0xff;
            const auto target = system->completedFrames() + 3;
            while (system->completedFrames() < target) step(*system, input.pad);
            if (system->readMemory(0xc002) != input.dc
                || system->cpu().registers().sp != 0xdff8 || system->readMemory(0xc000) < 2) return false;
            const auto pixels = system->framePixels();
            for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
                const auto pixel = pixels[y * 256 + x];
                const bool colored = (x + y) % 2 == 0;
                if (pixel.r != (colored && !pressed ? 255 : 0)
                    || pixel.g != (colored && pressed ? 255 : 0)
                    || pixel.b != (colored ? 0 : 255) || pixel.a != 255) return false;
            }
        }
    }
    return true;
}

bool prefixError() {
    auto system = std::make_unique<EmulatorSystem>(image(smsgg::test::stage5PrefixRom(), smsgg::MapperKind::Sega), VideoStandard::Ntsc, 48000);
    for (unsigned count = 0; count < 10000; ++count) {
        const auto registers = system->cpu().registers();
        const auto time = system->tstates(), frames = system->completedFrames();
        const auto result = system->step();
        if (std::holds_alternative<smsgg::CpuStepError>(result))
            return std::get<smsgg::CpuStepError>(result) == smsgg::CpuStepError::PrefixRun
                && system->tstates() == time && system->completedFrames() == frames
                && smsgg::test::sameRegisters(system->cpu().registers(), registers);
    }
    return false;
}

bool pauseIndicator() {
    auto system = std::make_unique<EmulatorSystem>(image(smsgg::test::stage5InputRom()), VideoStandard::Ntsc, 48000);
    while (system->completedFrames() < 3) step(*system);
    if (system->framePixels()[0].r != 255 || system->framePixels()[0].g != 0) return false;
    for (unsigned request = 1; request <= 2; ++request) {
        if (step(*system, {}, true) != 11) return false;
        const auto target = system->completedFrames() + 3;
        while (system->completedFrames() < target) step(*system);
        if (system->readMemory(0xc001) != request) return false;
        for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
            const bool colored = (x + y) % 2 == 0;
            const auto pixel = system->framePixels()[y * 256 + x];
            if (pixel.r != (colored ? 255 : 0) || pixel.g != (colored && request == 1 ? 255 : 0)
                || pixel.b != (colored ? 0 : 255) || pixel.a != 255) return false;
        }
    }
    return true;
}

bool sg1000(const std::filesystem::path& output) {
    using smsgg::Machine;
    auto bytes = smsgg::test::sg1000TestRom();
    if (!output.empty()) {
        std::filesystem::create_directories(output.parent_path());
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) return false;
    }
    auto rom = smsgg::RomLoader::fromBytes(std::move(bytes),
        {.mapper = smsgg::MapperKind::RomOnly, .machine = Machine::Sg1000});
    auto system = std::make_unique<EmulatorSystem>(std::get<smsgg::RomImage>(std::move(rom)),
        VideoStandard::Ntsc, 48000, Machine::Sg1000);
    smsgg::PadState pad{},player2{};
    bool positive = false, negative = false;
    const auto runFrames = [&](unsigned count) {
        positive = negative = false;
        const auto end = system->completedFrames() + count;
        while (system->completedFrames() < end) {
            if (!std::holds_alternative<uint32_t>(system->step(pad,false,player2))) return false;
            for (const auto pcm : system->audioFrames()) {
                if (pcm.left != pcm.right) return false;
                if (system->completedFrames() + 1 < end) continue;
                positive |= pcm.left > 0.1f;
                negative |= pcm.left < -0.1f;
            }
        }
        return true;
    };
    const auto pixelIs = [&](size_t index, uint8_t red, uint8_t green, uint8_t blue) {
        const auto pixel = system->framePixels()[index];
        return pixel.r == red && pixel.g == green && pixel.b == blue;
    };
    const auto background = [&](uint8_t red, uint8_t green, uint8_t blue) {
        return pixelIs(255, red, green, blue);
    };
    if (!runFrames(3) || !background(33, 200, 66) || !pixelIs(0, 255, 255, 255) || !positive || !negative
        || system->readMemory(0xc001) != 3 || system->readMemory(0xc002) != 0xe4
        || system->readMemory(0xc000) != 0xff) return false;
    pad = {.up = true, .button1 = true, .button2 = true};
    if (!runFrames(2) || !background(252, 85, 84) || !pixelIs(0, 84, 85, 237) || system->readMemory(0xfc00) != 0xce)
        return false;
    pad = {.down = true};
    if (!runFrames(2) || !pixelIs(5, 33, 200, 66) || !pixelIs(6, 255, 255, 255)) return false;
    pad.up = true;
    if (!runFrames(2) || !pixelIs(3, 252, 85, 84) || !pixelIs(4, 33, 200, 66)) return false;
    if (step(*system, pad, true) != 11 || system->cpu().registers().pc != 0x66 || !runFrames(2)
        || system->readMemory(0xc003) != 1 || !background(84, 85, 237) || positive || negative)
        return false;
    pad = {};
    if (!(step(*system, pad, true) == 11 && runFrames(2) && system->readMemory(0xc003) == 0
        && system->readMemory(0xfc00) == 0xff && background(33, 200, 66) && positive && negative)) return false;
    for(unsigned bits=0;bits<64;++bits) {
        player2={bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8),bool(bits&16),bool(bits&32)};
        pad.button2=(bits&1)!=0;
        if(!runFrames(2)) return false;
        const auto even=0xff ^ ((bits&3)<<6) ^ (pad.button2?0x20:0);
        const auto odd=0xff ^ (bits>>2);
        if(system->readMemory(0xc000)!=even || system->readMemory(0xc004)!=even ||
            system->readMemory(0xc005)!=odd || system->readMemory(0xc006)!=odd) return false;
    }
    player2={};
    pad={};
    return runFrames(2) && system->readMemory(0xc004)==0xff && system->readMemory(0xc006)==0xff;
}
}

int main(int argc, char** argv) {
    const std::string_view name = argc >= 2 ? argv[1] : "";
    const bool pass = name == "clock" ? clockProgression() : name == "interrupts" ? interrupts()
        : name == "interrupt_modes" ? interruptModes()
        : name == "sms_boot_vdp" ? smsBootVdp()
        : name == "rom" ? rom(argc == 3 ? argv[2] : "") : name == "pause_indicator" ? pauseIndicator()
        : name == "sg1000" ? sg1000(argc == 3 ? argv[2] : "")
        : name == "prefix_error" && prefixError();
    if (!pass) std::cerr << "EmulatorSystem test failed: " << name << '\n';
    return pass ? 0 : 1;
}
