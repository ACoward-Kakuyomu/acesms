#include "core/input/Controller.h"
#include "core/bus/IoBus.h"
#include "core/bus/MemoryControl.h"
#include "core/bus/SmsIo.h"
#include "core/bus/VdpIo.h"
#include "core/video/Vdp.h"
#include "../cpu/Z80MemoryTestSupport.h"

#include <iostream>
#include <string_view>

namespace {
smsgg::PadState state(unsigned buttons) {
    return {bool(buttons & 1), bool(buttons & 2), bool(buttons & 4),
        bool(buttons & 8), bool(buttons & 16), bool(buttons & 32)};
}

bool buttons() {
    smsgg::Controller controller;
    if (controller.readDc() != 0xff || controller.readDd() != 0xff) return false;
    for (unsigned bits = 0; bits < 64; ++bits) {
        controller.setState(state(bits));
        if (controller.readDc() != (0xff ^ bits) || controller.readDd() != 0xff) return false;
    }
    controller.setState({});
    return controller.readDc() == 0xff;
}

bool control() {
    smsgg::Controller controller;
    for (unsigned bits = 0; bits < 64; ++bits) {
        controller.setState(state(bits));
        for (unsigned value = 0; value < 256; ++value) {
            controller.writeControl(static_cast<uint8_t>(value));
            // A pin configured as input uses the external level; output uses its latch.
            const bool aTr = value & 1 ? !(bits & 32) : bool(value & 16);
            const bool aTh = value & 2 ? true : bool(value & 32);
            const bool bTr = value & 4 ? true : bool(value & 64);
            const bool bTh = value & 8 ? true : bool(value & 128);
            const unsigned dc = (0xdf ^ (bits & 31)) | (aTr ? 32 : 0);
            const unsigned dd = 0x37 | (bTr ? 8 : 0) | (aTh ? 64 : 0) | (bTh ? 128 : 0);
            if (controller.readDc() != dc || controller.readDd() != dd) return false;
        }
    }
    return true;
}

bool bus() {
    smsgg::test::Memory cartridge;
    cartridge.bytes.fill(0xa5);
    smsgg::MemoryBus memory(cartridge);
    smsgg::Controller controller;
    smsgg::AudioMixer audio(3579545,48000);
    smsgg::SmsIo sms(controller,audio);
    smsgg::Vdp vdp(smsgg::VideoStandard::Ntsc);
    smsgg::VdpIo video(vdp, sms);
    smsgg::MemoryControl controls(memory, video);
    smsgg::IoBus io(controls);
    controller.setState({.up = true, .button2 = true}, {.up = true, .right = true, .button2 = true});
    for (unsigned high = 0; high < 256; ++high) {
        for (unsigned port = 0xc0; port < 256; ++port) {
            if (io.readPort(static_cast<uint16_t>((high << 8) | port)) != (port % 2 ? 0xf5 : 0x9e)) return false;
        }
    }
    io.writePort(0x1201, 0x0e); // Mirrored 3F: A TR output low, other pins input.
    if (io.readPort(0xdc) != 0x9e || io.readPort(0xdd) != 0xf5) return false;
    io.writePort(0x003f, 0x5a); // Both TR outputs high, overriding button 2 on both pads.
    if (io.readPort(0xdc) != 0xbe || io.readPort(0xdd) != 0xfd) return false;
    // 3E still controls memory, and is not read back as a latch.
    io.writePort(0x3e, 0x40);
    if (memory.read8(0) != 0xff || io.readPort(0x3e) != 0xff || io.readPort(0x3f) != 0xff) return false;
    io.writePort(0x3e, 0);
    if (memory.read8(0) != 0xa5) return false;
    // VDP control/data/status and counters still reach the real VDP.
    io.writePort(0xbf, 0); io.writePort(0xbf, 0x40); io.writePort(0xbe, 0x5a);
    io.writePort(0xbf, 0); io.writePort(0xbf, 0);
    if (io.readPort(0xbe) != 0x5a || io.readPort(0x7e) != vdp.readVCounter()
        || io.readPort(0x7f) != vdp.readHCounter()) return false;
    io.writePort(0x7f, 0x80); // PSG write must not change VDP/controller state.
    return io.readPort(0xdc) == 0xbe && io.readPort(0xdd) == 0xfd;
}
}

int main(int argc, char** argv) {
    const std::string_view name = argc == 2 ? argv[1] : "";
    const bool pass = name == "buttons" ? buttons() : name == "control" ? control() : name == "bus" && bus();
    if (!pass) std::cerr << "Controller test failed: " << name << '\n';
    return pass ? 0 : 1;
}
