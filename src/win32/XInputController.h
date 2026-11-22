#pragma once

#include "win32/InputManager.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>

namespace smsgg::win32 {
class XInputController final {
public:
    using ReadState = decltype(&::XInputGetState);
    explicit XInputController(ReadState readState = &::XInputGetState) : readState_(readState) {}
    GamepadState poll(unsigned slot) const;
private:
    ReadState readState_;
};
} // namespace smsgg::win32
