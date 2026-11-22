#include "VdpTestSupport.h"

#include <iostream>
#include <string_view>

using smsgg::Vdp;
using smsgg::VideoStandard;
using smsgg::test::command;

namespace {
bool roundtrip() {
    Vdp vdp(VideoStandard::Ntsc);
    for (const unsigned pattern : {0, 0x55, 0xaa, 0xff}) {
        command(vdp, 0, 1);
        for (unsigned i = 0; i < 16384; ++i) vdp.writeData(static_cast<uint8_t>(i ^ (i / 256) ^ pattern));
        command(vdp, 0, 0);
        for (unsigned i = 0; i < 16384; ++i) {
            if (vdp.readData() != static_cast<uint8_t>(i ^ (i / 256) ^ pattern)) {
                std::cerr << "VRAM offset=" << i << " pattern=" << pattern << '\n'; return false;
            }
        }
    }
    return true;
}
bool initial() {
    Vdp first(VideoStandard::Ntsc), second(VideoStandard::Pal);
    command(first, 0, 1); first.writeData(0xa5);
    command(second, 0, 0);
    for (unsigned i = 0; i < 16384; ++i) if (second.readData() != 0) return false;
    command(first, 0, 0);
    return first.readData() == 0xa5;
}
bool wrap() {
    Vdp vdp(VideoStandard::Ntsc);
    command(vdp, 0x3fff, 1); vdp.writeData(0x12); vdp.writeData(0x34); vdp.writeData(0x56);
    command(vdp, 0x3fff, 0);
    return vdp.readData() == 0x12 && vdp.readData() == 0x34 && vdp.readData() == 0x56;
}
bool buffer() {
    Vdp vdp(VideoStandard::Ntsc);
    command(vdp, 0, 1);
    for (unsigned i = 0; i < 16384; ++i) vdp.writeData(static_cast<uint8_t>(i ^ (i / 256)));
    for (unsigned code = 0; code < 4; ++code) {
        command(vdp, 0x1234, 0); // Buffer=26, address=1235.
        command(vdp, 0x2345, static_cast<uint8_t>(code));
        if (vdp.readData() != (code == 0 ? 0x66 : 0x26)
            || vdp.readData() != (code == 0 ? 0x65 : 0x66)) return false;
    }
    command(vdp, 0x1200, 0);
    vdp.writeControl(0x56); // Change the low address immediately, without triggering prefetch.
    if (vdp.readData() != 0x12 || vdp.readData() != 0x44) return false;
    command(vdp, 0x1234, 1); vdp.writeData(0xab);
    if (vdp.readData() != 0xab || vdp.readData() != 0x27) return false;
    return true;
}
bool latch() {
    for (unsigned operation = 0; operation < 3; ++operation) {
        Vdp vdp(VideoStandard::Ntsc);
        command(vdp, 0, 1); vdp.writeData(0xa5);
        command(vdp, 0, 0);
        vdp.writeControl(0x55);
        if (operation == 0) vdp.readData();
        else if (operation == 1) vdp.writeData(0x69);
        else if (vdp.readStatus() != 0x1f) return false;
        command(vdp, 0x0100, 1); vdp.writeData(0x7b);
        command(vdp, 0x0100, 0);
        if (vdp.readData() != 0x7b) return false;
        command(vdp, 0, 0);
        vdp.writeControl(0x55); vdp.readStatus();
        if (vdp.readData() != 0xa5) return false; // Status preserves the read buffer.
    }
    return true;
}
bool codeWrites() {
    for (const uint8_t code : {0,1,2}) {
        Vdp vdp(VideoStandard::Ntsc);
        command(vdp, 0x2100, 1); vdp.writeData(0x11); vdp.writeData(0x22);
        command(vdp, 0x2100, code);
        vdp.readStatus(); // Preserve address/code while clearing only the command phase.
        vdp.writeData(0x57);
        command(vdp, 0x2100, 0);
        if (vdp.readData() != (code == 0 ? 0x11 : 0x57) || vdp.readData() != (code == 0 ? 0x57 : 0x22)) return false;
    }
    return true;
}
} // namespace
int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "roundtrip") return roundtrip() ? 0 : 1;
        if (name == "initial") return initial() ? 0 : 1;
        if (name == "wrap") return wrap() ? 0 : 1;
        if (name == "buffer") return buffer() ? 0 : 1;
        if (name == "latch") return latch() ? 0 : 1;
        if (name == "code_writes") return codeWrites() ? 0 : 1;
    }
    std::cerr << "Failed VDP port test\n";
    return 1;
}
