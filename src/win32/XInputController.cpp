#include "win32/XInputController.h"

namespace smsgg::win32 {
GamepadState XInputController::poll(unsigned slot) const {
    XINPUT_STATE state{};
    if (readState_(slot, &state) != ERROR_SUCCESS) return {};
    const auto& raw = state.Gamepad;
    const auto buttons = raw.wButtons;
    constexpr SHORT zone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE;
    return {{(buttons & XINPUT_GAMEPAD_DPAD_UP) != 0 || raw.sThumbLY > zone,
        (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 || raw.sThumbLY < -zone,
        (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 || raw.sThumbLX < -zone,
        (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 || raw.sThumbLX > zone},
        buttons,true};
}
} // namespace smsgg::win32
