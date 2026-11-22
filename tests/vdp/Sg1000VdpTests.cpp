#include "VdpTestSupport.h"
#include "core/bus/IoBus.h"
#include "core/bus/MemoryBus.h"
#include "core/bus/VdpIo.h"
#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"
#include "../bus/IoTestSupport.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <array>
#include <string_view>

namespace {
using smsgg::Machine;
using smsgg::Vdp;
using smsgg::VideoStandard;
using smsgg::test::command;
using smsgg::test::reg;

bool ports() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    smsgg::test::Io otherDevices;
    smsgg::VdpIo vdpIo(vdp, otherDevices);
    smsgg::IoBus io(vdpIo, Machine::Sg1000);

    io.writePort(0x1181, 0x80);
    io.writePort(0x2281, 0xc9); // Register 1, despite the extra select bits.
    io.writePort(0x1181, 0x12);
    io.readPort(0x2281); // Abandon the first byte of a command.
    io.writePort(0x1181, 0xff);
    io.writePort(0x2281, 0x7f);
    io.writePort(0x5580, 0x34);
    io.writePort(0x5580, 0x56);
    command(vdp, 0x3fff, 0);
    if (io.readPort(0xbe) != 0x34 || io.readPort(0xbe) != 0x56) return false;

    command(vdp, 0x0080, 1); vdp.writeData(0x11);
    command(vdp, 0x0040, 1); vdp.writeData(0x22);
    reg(vdp, 1, 0);
    command(vdp, 0x0040, 0);
    if (vdp.readData() != 0x11) return false;
    command(vdp, 0x1000, 0);
    if (vdp.readData() != 0x22) return false;
    command(vdp, 0x0040, 1); vdp.writeData(0x33);
    reg(vdp, 1, 0x80);
    command(vdp, 0x0080, 0);
    return vdp.readData() == 0x33 && otherDevices.accesses.empty();
}

bool irq() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    smsgg::test::Io otherDevices;
    smsgg::VdpIo vdpIo(vdp, otherDevices);
    smsgg::IoBus io(vdpIo, Machine::Sg1000);
    smsgg::test::Memory memory;
    smsgg::Z80 cpu(smsgg::Z80Registers{});
    auto state = cpu.captureState();
    state.iff1 = state.iff2 = true;
    state.interruptMode = 1;
    state.registers.sp = 0xc400;
    cpu.restoreState(state);

    reg(vdp, 0, 0x10); // The SMS line-IRQ enable bit is unused.
    constexpr uint64_t vblank = 192 * 228;
    constexpr uint64_t frame = 262 * 228;
    vdp.advanceTo(vblank - 1);
    if (vdp.interruptPending() || (io.readPort(0xbf) & 0xe0) != 0) return false;
    vdp.advanceTo(vblank);
    if (vdp.interruptPending()) return false;
    reg(vdp, 1, 0x20);
    if (!vdp.interruptPending() || !cpu.canAcceptMaskableInterrupt()) return false;
    constexpr std::array<uint8_t, 1> acknowledge{0xff};
    if (cpu.acceptInterrupt(acknowledge, &memory, &io) != 13 || cpu.registers().pc != 0x38)
        return false;
    if ((io.readPort(0xbf) & 0xe0) != 0x80 || vdp.interruptPending()) return false;
    vdp.advanceTo(frame + vblank);
    return vdp.completedFrames() == 1 && vdp.interruptPending();
}

constexpr uint64_t frame = 262 * 228;

bool pixelIs(const Vdp& vdp, unsigned x, unsigned y, smsgg::Rgba8 expected) {
    const auto pixel = vdp.framePixels()[y * 256 + x];
    return pixel.r == expected.r && pixel.g == expected.g && pixel.b == expected.b && pixel.a == 255;
}

void writeVram(Vdp& vdp, uint16_t address, std::initializer_list<uint8_t> bytes) {
    command(vdp, address, 1);
    for (const auto value : bytes) vdp.writeData(value);
}

bool graphicsI() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    smsgg::test::Io otherDevices;
    smsgg::VdpIo vdpIo(vdp, otherDevices);
    smsgg::IoBus io(vdpIo, Machine::Sg1000);
    std::vector<uint8_t> program;
    auto output = [&](uint8_t port, uint8_t value) {
        program.insert(program.end(), {0x3e, value, 0xd3, port}); // LD A,n; OUT (port),A
    };
    for (const auto [index, value] : std::array<std::pair<uint8_t, uint8_t>, 5>{{
             {1, 0xc0}, {2, 14}, {3, 132}, {4, 1}, {7, 8},
         }}) {
        output(0xbf, value);
        output(0xbf, static_cast<uint8_t>(0x80 | index));
    }
    for (const auto [address, value] : std::array<std::pair<uint16_t, uint8_t>, 3>{{
             {0x3800, 8}, {0x0840, 0xa0}, {0x2101, 0x20},
         }}) {
        output(0xbf, static_cast<uint8_t>(address));
        output(0xbf, static_cast<uint8_t>(0x40 | (address >> 8)));
        output(0xbe, value);
    }
    const auto end = program.size();
    program.resize(8192);
    auto image = smsgg::RomLoader::fromBytes(std::move(program),
        {.mapper = smsgg::MapperKind::RomOnly, .machine = Machine::Sg1000});
    smsgg::Cartridge cartridge(std::get<smsgg::RomImage>(std::move(image)));
    smsgg::MemoryBus bus(cartridge, Machine::Sg1000);
    smsgg::Z80 cpu(smsgg::Z80Registers{});
    uint64_t elapsed = 0;
    while (cpu.registers().pc < end) {
        const auto result = cpu.step(bus, io);
        const auto* cycles = std::get_if<uint32_t>(&result);
        if (!cycles) return false;
        elapsed += *cycles;
        vdp.advanceTo(elapsed);
    }
    vdp.advanceTo(2 * frame);
    constexpr smsgg::Rgba8 green{33, 200, 66}, red{252, 85, 84};
    if (!pixelIs(vdp, 0, 0, green) || !pixelIs(vdp, 1, 0, red) || !pixelIs(vdp, 2, 0, green)
        || !pixelIs(vdp, 8, 0, red) || !pixelIs(vdp, 0, 1, red)) return false;
    reg(vdp, 1, 0x80);
    vdp.advanceTo(3 * frame);
    return pixelIs(vdp, 0, 0, red);
}

bool graphicsII() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    reg(vdp, 0, 2); reg(vdp, 1, 0xc0); reg(vdp, 2, 14);
    reg(vdp, 3, 0xff); reg(vdp, 4, 3);
    writeVram(vdp, 0x0000, {0x80}); writeVram(vdp, 0x0800, {0x40}); writeVram(vdp, 0x1000, {0x20});
    writeVram(vdp, 0x2000, {0xf1}); writeVram(vdp, 0x2800, {0x21}); writeVram(vdp, 0x3000, {0x81});
    vdp.advanceTo(frame);
    constexpr smsgg::Rgba8 white{255, 255, 255}, green{33, 200, 66}, red{252, 85, 84}, black{};
    if (!pixelIs(vdp, 0, 0, white) || !pixelIs(vdp, 0, 64, black) || !pixelIs(vdp, 1, 64, green)
        || !pixelIs(vdp, 2, 128, red)) return false;
    reg(vdp, 4, 0);
    vdp.advanceTo(2 * frame);
    if (!pixelIs(vdp, 0, 128, red) || !pixelIs(vdp, 2, 128, black)) return false;
    reg(vdp, 3, 0x80);
    vdp.advanceTo(3 * frame);
    return pixelIs(vdp, 0, 64, white) && pixelIs(vdp, 0, 128, white);
}

bool textMode() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    reg(vdp, 1, 0xd0); reg(vdp, 2, 14); reg(vdp, 4, 1); reg(vdp, 7, 0xf2);
    writeVram(vdp, 0x3800, {1}); writeVram(vdp, 0x3827, {1});
    writeVram(vdp, 0x0808, {0x84});
    vdp.advanceTo(frame);
    constexpr smsgg::Rgba8 white{255, 255, 255}, green{33, 200, 66};
    return pixelIs(vdp, 5, 0, green) && pixelIs(vdp, 6, 0, white)
        && pixelIs(vdp, 11, 0, white) && pixelIs(vdp, 12, 0, green)
        && pixelIs(vdp, 240, 0, white) && pixelIs(vdp, 245, 0, white) && pixelIs(vdp, 246, 0, green);
}

bool multicolor() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    reg(vdp, 1, 0xc8); reg(vdp, 2, 14); reg(vdp, 4, 1); reg(vdp, 7, 1);
    writeVram(vdp, 0x3800, {1}); writeVram(vdp, 0x3820, {1});
    writeVram(vdp, 0x0808, {0x2f, 0x48, 0x82, 0xf0});
    vdp.advanceTo(frame);
    constexpr smsgg::Rgba8 white{255, 255, 255}, green{33, 200, 66},
        blue{84, 85, 237}, red{252, 85, 84}, black{};
    return pixelIs(vdp, 3, 3, green) && pixelIs(vdp, 4, 3, white)
        && pixelIs(vdp, 3, 4, blue) && pixelIs(vdp, 4, 4, red)
        && pixelIs(vdp, 0, 8, red) && pixelIs(vdp, 4, 8, green)
        && pixelIs(vdp, 0, 12, white) && pixelIs(vdp, 4, 12, black);
}

bool sprites() {
    Vdp vdp(VideoStandard::Ntsc, Machine::Sg1000);
    reg(vdp, 1, 0xc0); reg(vdp, 4, 1); reg(vdp, 5, 0x20); reg(vdp, 6, 4); reg(vdp, 7, 1);
    writeVram(vdp, 0x1000, {
        0xff, 5, 0, 2, 0xff, 5, 0, 15, 0xff, 40, 0, 0x84, 0xff, 12, 0, 0,
        0x40, 0, 0, 2, 0xff, 16, 0, 15, 0xd0, 0, 0, 0, 0xff, 20, 0, 2,
    });
    writeVram(vdp, 0x2000, {0x80});
    vdp.advanceTo(228);
    if (vdp.readStatus() != 0x65 || vdp.readStatus() != 0x1f) return false;
    vdp.advanceTo(frame);
    constexpr smsgg::Rgba8 green{33, 200, 66}, blue{84, 85, 237}, black{};
    if (!pixelIs(vdp, 5, 0, green) || !pixelIs(vdp, 8, 0, blue) || !pixelIs(vdp, 12, 0, black)
        || !pixelIs(vdp, 16, 0, black) || !pixelIs(vdp, 20, 0, black)) return false;

    vdp.readStatus();
    reg(vdp, 1, 0xd0);
    vdp.advanceTo(2 * frame);
    if (!pixelIs(vdp, 5, 0, black) || (vdp.readStatus() & 0x60) != 0) return false;

    reg(vdp, 1, 0xc3);
    writeVram(vdp, 0x1000, {0xff, 0, 7, 2, 0xd0});
    writeVram(vdp, 0x2020, {0x80});
    writeVram(vdp, 0x2038, {0x80});
    vdp.advanceTo(3 * frame);
    if (!pixelIs(vdp, 0, 0, green) || !pixelIs(vdp, 1, 1, green) || !pixelIs(vdp, 0, 2, black)
        || !pixelIs(vdp, 16, 16, green) || !pixelIs(vdp, 17, 17, green)
        || !pixelIs(vdp, 17, 18, black)) return false;

    vdp.readStatus();
    reg(vdp, 1, 0xc0);
    writeVram(vdp, 0x1000, {0xff, 31, 0, 0x80, 0xff, 31, 0, 0x82, 0xd0});
    vdp.advanceTo(3 * frame + 228);
    return vdp.readStatus() == 0x3f && vdp.readStatus() == 0x1f;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) return 1;
    const std::string_view test(argv[1]);
    if (test == "ports") return ports() ? 0 : 1;
    if (test == "irq") return irq() ? 0 : 1;
    if (test == "graphics_i") return graphicsI() ? 0 : 1;
    if (test == "graphics_ii") return graphicsII() ? 0 : 1;
    if (test == "text") return textMode() ? 0 : 1;
    if (test == "multicolor") return multicolor() ? 0 : 1;
    if (test == "sprites") return sprites() ? 0 : 1;
    return 1;
}
