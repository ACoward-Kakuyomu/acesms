#include "core/bus/VdpIo.h"
#include "core/bus/IoBus.h"
#include "core/bus/MemoryControl.h"
#include "core/bus/SmsIo.h"
#include "core/cartridge/Cartridge.h"
#include "core/cartridge/RomLoader.h"
#include "core/state/StateCodec.h"
#include "IoTestSupport.h"
#include "BusTestSupport.h"
#include "../vdp/VdpTestSupport.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string_view>

using smsgg::test::command;
using smsgg::test::initialRegisters;
using smsgg::test::sameRegisters;
namespace {
struct Rig {
    smsgg::Cartridge cartridge;
    smsgg::MemoryBus memory;
    smsgg::test::Io devices;
    smsgg::Vdp vdp;
    smsgg::VdpIo vdpIo;
    smsgg::MemoryControl control;
    smsgg::IoBus io;
    smsgg::test::TraceMemory trace;
    uint64_t cycle{};
    explicit Rig(smsgg::VideoStandard video = smsgg::VideoStandard::Ntsc)
        : cartridge(std::get<smsgg::RomImage>(smsgg::RomLoader::fromBytes(std::vector<uint8_t>(8192, 0x11), {smsgg::MapperKind::RomOnly}))),
          memory(cartridge), vdp(video), vdpIo(vdp, devices), control(memory, vdpIo), io(control), trace(memory) {}
    std::optional<uint32_t> execute(smsgg::Z80& cpu, std::initializer_list<uint8_t> bytes) {
        const auto time = cpu.executeInstruction({bytes.begin(), bytes.size()}, &trace, &io, cycle);
        if (time) { cycle += *time; vdp.advanceTo(cycle); }
        return time;
    }
    std::optional<uint32_t> interrupt(smsgg::Z80& cpu) {
        const auto time = vdp.interruptPending() ? cpu.acceptInterrupt({}, &trace, &io) : std::optional<uint32_t>{0};
        if (time) { cycle += *time; vdp.advanceTo(cycle); }
        return time;
    }
};
bool out(Rig& rig, smsgg::Z80& cpu, smsgg::Z80Registers& expected, uint8_t port, uint8_t value) {
    const auto load = rig.execute(cpu, {0x3e, value});
    expected.setA(value); expected.pc = static_cast<uint16_t>(expected.pc + 2);
    expected.r = static_cast<uint8_t>((expected.r & 0x80) | ((expected.r + 1) & 0x7f));
    if (!load || *load != 7 || !sameRegisters(cpu.registers(), expected)) return false;
    const auto time = rig.execute(cpu, {0xd3, port});
    expected.pc = static_cast<uint16_t>(expected.pc + 2);
    expected.r = static_cast<uint8_t>((expected.r & 0x80) | ((expected.r + 1) & 0x7f));
    return time && *time == 11 && sameRegisters(cpu.registers(), expected);
}
bool ports() {
    auto vdp = std::make_unique<smsgg::Vdp>(smsgg::VideoStandard::Ntsc);
    smsgg::test::Memory external; smsgg::MemoryBus memory(external);
    smsgg::test::Io devices;
    smsgg::VdpIo vdpIo(*vdp, devices); smsgg::MemoryControl control(memory, vdpIo); smsgg::IoBus io(control);
    vdp->advanceTo(7 * 228 + 198); vdp->latchHCounter();
    for (unsigned port = 0; port < 65536; ++port) for (const uint8_t value : {0,0x55,0xaa,0xff}) {
        io.writePort(0x3e, 0); // Port delegation cases are independent of the previous 3E write.
        const auto address = static_cast<uint16_t>(port);
        const unsigned low = port % 256;
        vdp->readStatus(); devices.reply = value; devices.accesses.clear();
        command(*vdp, 0x1234, 1); vdp->writeData(static_cast<uint8_t>(value ^ 0xa5));
        const uint8_t expected = low < 64 || low >= 192 ? value : low < 128 ? (low % 2 ? 0xea : 7)
            : low % 2 ? 0x1f : static_cast<uint8_t>(value ^ 0xa5);
        if (io.readPort(address) != expected) return false;
        const bool delegated = low < 64 || low >= 192;
        if (devices.accesses.size() != (delegated ? 1 : 0)) return false;
        if (delegated) {
            const uint16_t representative = static_cast<uint16_t>((low < 64 ? 0x3e : 0xdc) + low % 2);
            const auto access = devices.accesses[0];
            if (access.write || access.port != representative || access.value != value) return false;
        }
        vdp->readStatus(); devices.accesses.clear();
        if (low >= 128 && low < 192 && low % 2) {
            io.writePort(address, 0x34); io.writePort(address, 0x52); vdp->writeData(value);
        } else {
            command(*vdp, 0x1234, 1); io.writePort(address, value);
        }
        const int destination = low < 64 ? (low % 2 ? 0x3f : -1) : low < 128 ? 0x7f : -1;
        if (devices.accesses.size() != (destination < 0 ? 0 : 1)) return false;
        if (destination >= 0) {
            const auto access = devices.accesses[0];
            if (!access.write || access.port != destination || access.value != value) return false;
        }
        if (low >= 128 && low < 192) { command(*vdp, 0x1234, 0); if (vdp->readData() != value) return false; }
    }
    return true;
}
bool memoryControl() {
    auto vdp = std::make_unique<smsgg::Vdp>(smsgg::VideoStandard::Ntsc);
    smsgg::test::Memory external; external.bytes[0x4000] = 0xa5;
    smsgg::MemoryBus memory(external); memory.write8(0xc000, 0x59);
    smsgg::test::Io devices; devices.reply = 0x6b;
    smsgg::VdpIo vdpIo(*vdp, devices); smsgg::MemoryControl control(memory, vdpIo); smsgg::IoBus io(control);
    for (const uint8_t value : {0,0x10,0x40,0x50}) {
        io.writePort(0xff00, value);
        if (memory.read8(0x4000) != (value / 64 % 2 ? 0xff : 0xa5)
            || memory.read8(0xc000) != (value / 16 % 2 ? 0xff : 0x59)) return false;
        command(*vdp, 0, 1); io.writePort(0x80, 0x37); command(*vdp, 0, 0);
        if (io.readPort(0xbe) != 0x37 || io.readPort(0x3e) != 0x6b) return false;
    }
    return true;
}
bool thLatch() {
    using namespace smsgg;
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc);
    test::Memory cartridge;
    MemoryBus memory(cartridge);
    Controller controller;
    AudioMixer audio(3579545, 48000);
    SmsIo terminal(controller, audio, *vdp);
    VdpIo video(*vdp, terminal);
    MemoryControl control(memory, video);
    IoBus io(control);

    io.writePort(0x3f, 0x0d, 12); // A TH output low.
    if (io.readPort(0x7f) != 0) return false;
    io.writePort(0x1201, 0x2d, 24); // A rises via a mirrored control port.
    if (io.readPort(0x7f) != 0x12 || io.readPort(0x9941) != 0x12) return false;
    io.writePort(0x3f, 0x2d, 36); // Same level.
    io.writePort(0x3f, 0x07, 48); // A stays high; B falls.
    if (io.readPort(0x7f) != 0x12) return false;
    io.writePort(0x3e, 4, 52);
    io.writePort(0x3f, 0x87, 56); // B write blocked by memory control.
    if (io.readPort(0x7f) != 0x12) return false;
    io.writePort(0x3e, 0, 60);
    if ((io.readPort(0xdd) & 0x80) != 0) return false;
    io.writePort(0x3f, 0x0f, 64); // B rises via direction change to input.
    if (io.readPort(0x7f) != 0x30) return false;

    io.writePort(0x3f, 0x25, 80); // Both TH outputs; A high, B low.
    memory.write8(0xc000, 0x85); // OTIR makes B rise and A fall.
    Z80 cpu({.bc = 0x023f, .hl = 0xc000});
    vdp->advanceTo(100);
    constexpr uint8_t otir[]{0xed, 0xb3};
    if (cpu.executeInstruction(otir, &memory, &io, 100) != 21) return false;
    vdp->advanceTo(121);
    return io.readPort(0x7f) == 0x57 && io.readPort(0x7d) == 0x57;
}
bool cpuPorts() {
    for (const uint8_t port : {0x80,0xbe}) for (const uint8_t value : {0,0x55,0xaa,0xff}) {
        auto rig = std::make_unique<Rig>();
        auto expected = initialRegisters(); expected.pc = 0xffff; expected.r = 0xff;
        smsgg::Z80 cpu(expected);
        if (!out(*rig, cpu, expected, 0xbf, 0xff) || !out(*rig, cpu, expected, 0xbf, 0x7f)
            || !out(*rig, cpu, expected, port, value) || !out(*rig, cpu, expected, port, static_cast<uint8_t>(~value))
            || !out(*rig, cpu, expected, 0x81, 0xff) || !out(*rig, cpu, expected, 0x81, 0x3f)) return false;
        for (const uint8_t wanted : {value, static_cast<uint8_t>(~value)}) {
            const auto time = rig->execute(cpu, {0xdb, port});
            expected.setA(wanted); expected.pc = static_cast<uint16_t>(expected.pc + 2);
            expected.r = static_cast<uint8_t>((expected.r & 0x80) | ((expected.r + 1) & 0x7f));
            if (!time || *time != 11 || !sameRegisters(cpu.registers(), expected)) return false;
        }
        if (rig->cycle != 130 || rig->vdp.readVCounter() != 0 || !rig->trace.accesses.empty() || !rig->devices.accesses.empty()) return false;
    }
    return true;
}
bool vCounterReadTiming() {
    for (const unsigned phase : {211u, 212u, 213u}) {
        auto rig = std::make_unique<Rig>();
        rig->cycle = 218 * 228 + phase - 11;
        rig->vdp.advanceTo(rig->cycle);
        smsgg::Z80 cpu(initialRegisters());
        if (rig->execute(cpu, {0xdb, 0x40}) != 11
            || cpu.registers().a() != (phase == 211 ? 0xda : 0xd5)) return false;
    }
    // INIR finishes after the boundary, but should store the value at the earlier I/O cycle.
    auto rig = std::make_unique<Rig>();
    rig->cycle = 198;
    rig->vdp.advanceTo(rig->cycle);
    smsgg::Z80 cpu({.bc = 0x027e, .hl = 0xc000});
    return rig->execute(cpu, {0xed, 0xb2}) == 21 && rig->memory.read8(0xc000) == 0
        && rig->vdp.readVCounter() == 1
        && rig->execute(cpu, {0xed, 0xb2}) == 16 && rig->memory.read8(0xc001) == 1;
}
bool cpuBlock() {
    for (const bool output : {false,true}) {
        auto rig = std::make_unique<Rig>();
        rig->memory.write8(0xffff, 0x11);
        if (!output) { command(rig->vdp, 0x3fff, 1); rig->vdp.writeData(0x11); rig->vdp.writeData(0x11); }
        command(rig->vdp, 0x3fff, output ? 1 : 0);
        auto expected = initialRegisters(); expected.hl = 0xffff; expected.bc = output ? 0x02be : 0x0280;
        smsgg::Z80 cpu(expected);
        for (unsigned i = 0; i < 2; ++i) {
            const auto time = rig->execute(cpu, {0xed, static_cast<uint8_t>(output ? 0xb3 : 0xb2)});
            expected.hl = static_cast<uint16_t>(i); expected.setB(static_cast<uint8_t>(1 - i));
            expected.r = static_cast<uint8_t>(0x37 + i * 2); expected.pc = i == 0 ? 0x1234 : 0x1236;
            expected.setF(i == 0 ? 0 : 0x40);
            if (!time || *time != (i == 0 ? 21 : 16) || !sameRegisters(cpu.registers(), expected)) return false;
        }
        if (rig->cycle != 37 || rig->trace.accesses.size() != 2 || !rig->devices.accesses.empty()) return false;
        for (unsigned i = 0; i < 2; ++i) {
            const auto access = rig->trace.accesses[i];
            if (access.write == output || access.address != static_cast<uint16_t>(0xffff + i) || access.value != 0x11) return false;
        }
        command(rig->vdp, 0x3fff, 0);
        if (rig->vdp.readData() != 0x11 || rig->vdp.readData() != 0x11 || rig->memory.read8(0xffff) != 0x11) return false;
    }
    return true;
}
bool cramTiming(smsgg::VideoStandard video) {
    auto rig = std::make_unique<Rig>(video);
    auto& vdp = rig->vdp;
    smsgg::test::reg(vdp, 0, 4);
    smsgg::test::reg(vdp, 1, 0x40);
    smsgg::test::reg(vdp, 2, 0x0e);
    smsgg::test::reg(vdp, 5, 0x7e);
    command(vdp, 0, 1);
    for (unsigned row = 0; row < 8; ++row) {
        for (uint8_t plane : {0xff, 0, 0, 0}) vdp.writeData(plane);
    }
    command(vdp, 0x3f00, 1);
    vdp.writeData(0xd0);
    const auto write = [&](uint64_t time, uint8_t color) {
        rig->cycle = time - 11;
        vdp.advanceTo(rig->cycle);
        command(vdp, 1, 3);
        smsgg::Z80 cpu({.af = static_cast<uint16_t>(unsigned(color) << 8)});
        return rig->execute(cpu, {0xd3, 0x80}) == 11;
    };
    command(vdp, 1, 3);
    rig->io.writePort(0x80, 3, 0); // Red, x=0
    if (!write(11, 0x30) || !write(40, 0x0c) || !write(170, 0x3f)) return false;
    command(vdp, 1, 3);
    rig->io.writePort(0x80, 0x30, 171); // First invisible dot, carries blue into next line.
    if (!write(241, 0x3c) || !write(456, 3)) return false;
    vdp.advanceTo((video == smsgg::VideoStandard::Ntsc ? 262 : 313) * 228);
    const auto pixels = vdp.framePixels();
    const auto pixel = [&](unsigned y, unsigned x, unsigned rgb) {
        const auto c = pixels[y * 256 + x];
        return ((unsigned(c.r) << 16) | (unsigned(c.g) << 8) | c.b) == rgb;
    };
    return pixel(0, 0, 0xff0000) && pixel(0, 16, 0xff0000)
        && pixel(0, 17, 0x0000ff) && pixel(0, 59, 0x0000ff)
        && pixel(0, 60, 0x00ff00) && pixel(0, 254, 0x00ff00) && pixel(0, 255, 0xffffff)
        && pixel(1, 0, 0x0000ff) && pixel(1, 19, 0x0000ff) && pixel(1, 20, 0x00ffff)
        && pixel(1, 255, 0x00ffff) && pixel(2, 0, 0xff0000) && pixel(2, 255, 0xff0000);
}
bool cramRestore() {
    using namespace smsgg;
    std::vector<uint8_t> bytes(8192);
    constexpr uint8_t program[]{0x3e, 16, 0xd3, 0xbf, 0x3e, 0xc0, 0xd3, 0xbf,
        0x3e, 3, 0xd3, 0xbe, 0x76}; // 54 T-states to write red to backdrop CRAM.
    std::copy(std::begin(program), std::end(program), bytes.begin());
    const auto rom = std::get<RomImage>(RomLoader::fromBytes(std::move(bytes), {MapperKind::RomOnly}));
    const StateProfile profile{8192, rom.metadata().crc32, Machine::Sms, VideoStandard::Ntsc, MapperKind::RomOnly, 0, 48000};
    auto original = std::make_unique<EmulatorSystem>(rom, VideoStandard::Ntsc, 48000);
    auto restored = std::make_unique<EmulatorSystem>(rom, VideoStandard::Ntsc, 48000);
    while (original->tstates() < 54) original->step();
    auto state = std::make_unique<EmulatorSystemState>();
    original->captureState(*state);
    auto encoded = encodeState(profile, *state);
    auto decoded = decodeState(encoded, profile);
    auto* saved = std::get_if<std::unique_ptr<EmulatorSystemState>>(&decoded);
    if (!saved || restored->restoreState(**saved) != ACESMS_FM_OK) return false;
    while (!original->completedFrames()) if (original->step() != restored->step()) return false;
    const auto a = original->framePixels(), b = restored->framePixels();
    return a[80].r == 0 && a[81].r == 255 && std::equal(a.begin(), a.end(), b.begin(), [](Rgba8 x, Rgba8 y) {
        return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
    });
}
bool cpuIrq(bool lineSource = false) {
    auto rig = std::make_unique<Rig>();
    auto expected = initialRegisters(); expected.pc = 0x4000; expected.sp = 0xc100;
    smsgg::Z80 cpu(expected);
    if (rig->execute(cpu, {0xed,0x56}) != 8 || cpu.interruptMode() != 1) return false;
    expected.pc = 0x4002; expected.r = 0x37;
    if (!sameRegisters(cpu.registers(), expected)) return false;
    const uint8_t sourceRegister = lineSource ? 0 : 1, sourceEnable = lineSource ? 0x10 : 0x20;
    const uint8_t sourceStatus = lineSource ? 0x1f : 0x9f;
    smsgg::test::reg(rig->vdp, sourceRegister, sourceEnable);
    if (lineSource) smsgg::test::reg(rig->vdp, 10, 0);
    const uint64_t origin = lineSource ? 59736 - 16 : 43987;
    rig->cycle = origin; rig->vdp.advanceTo(rig->cycle); rig->vdp.readStatus();
    if (rig->execute(cpu, {0}) != 4 || !rig->vdp.interruptPending()) return false;
    expected.pc = 0x4003; expected.r = 0x38;
    if (rig->interrupt(cpu) != 0 || !sameRegisters(cpu.registers(), expected) || rig->cycle != origin + 4) return false;
    if (rig->execute(cpu, {0xfb}) != 4) return false;
    expected.pc = 0x4004; expected.r = 0x39;
    if (rig->interrupt(cpu) != 0 || !sameRegisters(cpu.registers(), expected) || cpu.canAcceptMaskableInterrupt()) return false;
    if (rig->execute(cpu, {0x76}) != 4 || !cpu.halted() || !cpu.canAcceptMaskableInterrupt()) return false;
    expected.pc = 0x4005; expected.r = 0x3a;
    if (!sameRegisters(cpu.registers(), expected) || rig->interrupt(cpu) != 13) return false;
    expected.pc = 0x38; expected.sp = 0xc0fe; expected.r = 0x3b;
    if (!sameRegisters(cpu.registers(), expected) || cpu.halted() || cpu.iff1() || cpu.iff2()
        || !rig->vdp.interruptPending() || rig->cycle != origin + 25 || rig->trace.accesses.size() != 2) return false;
    const auto high = rig->trace.accesses[0], low = rig->trace.accesses[1];
    if (!high.write || high.address != 0xc0ff || high.value != 0x40 || !low.write || low.address != 0xc0fe || low.value != 5) return false;
    if (rig->interrupt(cpu) != 0 || !sameRegisters(cpu.registers(), expected) || rig->cycle != origin + 25) return false;
    if (rig->execute(cpu, {0xdb,0xbf}) != 11 || rig->vdp.interruptPending()) return false;
    expected.pc = 0x3a; expected.r = 0x3c; expected.setA(sourceStatus);
    if (!sameRegisters(cpu.registers(), expected)) return false;
    if (rig->execute(cpu, {0xfb}) != 4 || rig->execute(cpu, {0xed,0x4d}) != 14) return false;
    expected.pc = 0x4005; expected.sp = 0xc100; expected.r = 0x3f;
    if (!sameRegisters(cpu.registers(), expected) || !cpu.canAcceptMaskableInterrupt() || rig->cycle != origin + 54) return false;
    if (rig->interrupt(cpu) != 0 || !sameRegisters(cpu.registers(), expected)) return false;
    // Leave the next source unacknowledged across EI/RETI: the same level must be accepted again.
    const uint64_t secondEdge = lineSource ? 59736 * 2 - 15 : 59736 + 43988;
    rig->cycle = secondEdge - 1; rig->vdp.advanceTo(rig->cycle); rig->vdp.readStatus();
    rig->cycle = secondEdge; rig->vdp.advanceTo(rig->cycle);
    if (rig->interrupt(cpu) != 13 || rig->execute(cpu, {0xfb}) != 4 || rig->execute(cpu, {0xed,0x4d}) != 14) return false;
    expected.r = 0x43;
    if (!sameRegisters(cpu.registers(), expected) || rig->interrupt(cpu) != 13) return false;
    expected.pc = 0x38; expected.sp = 0xc0fe; expected.r = 0x44;
    if (!sameRegisters(cpu.registers(), expected) || !rig->vdp.interruptPending()) return false;
    if (rig->execute(cpu, {0xfb}) != 4 || rig->execute(cpu, {0xed,0x4d}) != 14) return false;
    expected.pc = 0x4005; expected.sp = 0xc100; expected.r = 0x47;
    smsgg::test::reg(rig->vdp, sourceRegister, 0);
    if (rig->interrupt(cpu) != 0 || !sameRegisters(cpu.registers(), expected) || !cpu.canAcceptMaskableInterrupt()) return false;
    smsgg::test::reg(rig->vdp, sourceRegister, sourceEnable);
    if (rig->execute(cpu, {0xf3}) != 4) return false;
    ++expected.pc; expected.r = 0x48;
    if (rig->interrupt(cpu) != 0 || !sameRegisters(cpu.registers(), expected) || cpu.iff1() || cpu.iff2()) return false;
    if (rig->vdp.readStatus() != sourceStatus) return false;
    return !rig->vdp.interruptPending() && rig->devices.accesses.empty()
        && rig->cycle == secondEdge + 13 + 4 + 14 + 13 + 4 + 14 + 4;
}
bool frameStatusTiming() {
    using namespace smsgg;
    constexpr uint64_t request = 192 * 228 + 212;
    for (const uint64_t sample : {request, request + 1}) {
        auto rig = std::make_unique<Rig>();
        test::reg(rig->vdp, 1, 0x20);
        rig->cycle = sample - 11;
        rig->vdp.advanceTo(rig->cycle);
        Z80 cpu(initialRegisters());
        if (rig->execute(cpu, {0xdb, 0x81}) != 11
            || cpu.registers().a() != (sample == request ? 0x1f : 0x9f)
            || rig->vdp.interruptPending()) return false;
        rig->vdp.advanceTo(request + 1);
        if (rig->vdp.interruptPending() || rig->vdp.readStatus() != (sample == request ? 0x9f : 0x1f)) return false;
    }
    std::vector<uint8_t> bytes(8192);
    constexpr uint8_t enable[]{
        0x3e, 0x00, 0xd3, 0xbf, 0x3e, 0x80, 0xd3, 0xbf, // Disable Mode 4 sprite evaluation for this VINT test.
        0x3e, 0x20, 0xd3, 0xbf, 0x3e, 0x81, 0xd3, 0xbf,
    };
    std::copy(std::begin(enable), std::end(enable), bytes.begin());
    const auto rom = std::get<RomImage>(RomLoader::fromBytes(std::move(bytes), {MapperKind::RomOnly}));
    const StateProfile profile{8192, rom.metadata().crc32, Machine::Sms, VideoStandard::Ntsc, MapperKind::RomOnly, 0, 48000};
    auto system = std::make_unique<EmulatorSystem>(rom, VideoStandard::Ntsc, 48000);
    while (system->tstates() < request) system->step();
    auto snapshot = std::make_unique<EmulatorSystemState>();
    system->captureState(*snapshot);
    auto saved = decodeState(encodeState(profile, *snapshot), profile);
    const auto* restored = std::get_if<std::unique_ptr<EmulatorSystemState>>(&saved);
    if (!restored) return false;
    auto vdp = std::make_unique<Vdp>(VideoStandard::Ntsc);
    vdp->restoreState((*restored)->vdp);
    if (!vdp->interruptPending() || vdp->readStatus() != 0x1f || vdp->interruptPending()) return false;
    vdp->advanceTo(request + 1);
    return !vdp->interruptPending() && vdp->readStatus() == 0x9f;
}
} // namespace
int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "ports") return ports() ? 0 : 1;
        if (name == "memory_control") return memoryControl() ? 0 : 1;
        if (name == "th_latch") return thLatch() ? 0 : 1;
        if (name == "cpu_ports") return cpuPorts() ? 0 : 1;
        if (name == "v_counter_read_timing") return vCounterReadTiming() ? 0 : 1;
        if (name == "cpu_block") return cpuBlock() ? 0 : 1;
        if (name == "cram_timing") return cramTiming(smsgg::VideoStandard::Ntsc)
            && cramTiming(smsgg::VideoStandard::Pal) ? 0 : 1;
        if (name == "cram_restore") return cramRestore() ? 0 : 1;
        if (name == "cpu_irq") return cpuIrq() ? 0 : 1;
        if (name == "cpu_line_irq") return cpuIrq(true) ? 0 : 1;
        if (name == "frame_status_timing") return frameStatusTiming() ? 0 : 1;
    }
    std::cerr << "Failed VDP bus CPU test\n"; return 1;
}
