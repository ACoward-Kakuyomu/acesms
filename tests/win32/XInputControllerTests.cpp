#include "win32/XInputController.h"
#include "XInputTestSupport.h"

#include <iostream>
#include <string_view>

namespace {
namespace api = smsgg::test::xinput;
using smsgg::win32::XInputController;
unsigned mask(smsgg::PadState p) {
    return unsigned(p.up) | (p.down << 1) | (p.left << 2) | (p.right << 3)
        | (p.button1 << 4) | (p.button2 << 5);
}
bool mapping() {
    api::reset();
    api::results[0] = ERROR_SUCCESS;
    XInputController controller(api::read);
    constexpr WORD buttons[]{XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B};
    for (unsigned combination = 0; combination < 64; ++combination) {
        auto& raw = api::states[0].Gamepad;
        raw = {};
        for (unsigned bit = 0; bit < 6; ++bit) if (combination & (1u << bit)) raw.wButtons |= buttons[bit];
        const auto state = controller.poll(0);
        if (!state.connected || mask(state.pad) != (combination&15) || state.buttons!=raw.wButtons) return false;
    }
    auto& raw = api::states[0].Gamepad;
    raw = {};
    raw.wButtons = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
    const auto pause = controller.poll(0);
    if (pause.buttons!=raw.wButtons || mask(pause.pad)) return false;
    raw.wButtons = XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_LEFT_SHOULDER
        | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB | 0x0c00;
    raw.bLeftTrigger = raw.bRightTrigger = 255;
    raw.sThumbRX = raw.sThumbRY = 32767;
    const auto unused = controller.poll(0);
    return unused.connected && !mask(unused.pad) && unused.buttons==raw.wButtons;
}
bool sticks() {
    api::reset();
    api::results[0] = ERROR_SUCCESS;
    XInputController controller(api::read);
    struct Case { SHORT x, y; unsigned expected; };
    constexpr SHORT zone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    constexpr Case cases[]{{0,0,0},{zone,zone,0},{-zone,-zone,0},{zone+1,0,8},
        {-zone-1,0,4},{0,zone+1,1},{0,-zone-1,2},{32767,32767,9},{-32768,-32768,6}};
    for (const auto c : cases) {
        auto& raw = api::states[0].Gamepad;
        raw = {};
        raw.sThumbLX = c.x; raw.sThumbLY = c.y;
        if (mask(controller.poll(0).pad) != c.expected) return false;
    }
    api::states[0].Gamepad.wButtons = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_A;
    return mask(controller.poll(0).pad) == (6 | 1); // Preserve opposite directions and D-pad OR stick.
}
bool selection() {
    api::reset();
    XInputController controller(api::read);
    if (controller.poll(2).connected || api::calls != std::vector<DWORD>{2}) return false;
    api::results[0] = ERROR_SUCCESS;
    api::results[2] = ERROR_SUCCESS;
    api::states[2].Gamepad.wButtons = XINPUT_GAMEPAD_Y;
    api::calls.clear();
    if (controller.poll(2).buttons != XINPUT_GAMEPAD_Y || api::calls != std::vector<DWORD>{2}) return false;
    api::results[2] = ERROR_DEVICE_NOT_CONNECTED;
    const auto removed = controller.poll(2);
    if (removed.connected || mask(removed.pad) || removed.buttons || api::calls != std::vector<DWORD>{2,2}) return false;
    api::results[2] = ERROR_SUCCESS;
    return controller.poll(2).connected && api::calls == std::vector<DWORD>{2,2,2};
}
}
int main(int argc, char** argv) {
    const std::string_view name = argc == 2 ? argv[1] : "";
    const bool pass = name == "mapping" ? mapping() : name == "sticks" ? sticks() : name == "selection" && selection();
    if (!pass) std::cerr << "XInput test failed: " << name << '\n';
    return pass ? 0 : 1;
}
