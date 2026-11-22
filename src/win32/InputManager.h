#pragma once

#include "core/input/Controller.h"

#include <array>

namespace smsgg::win32 {
enum class InputAction { None, ConsolePause, TogglePause, ToggleFastForward };
enum class PadSource : unsigned { Disabled, Keyboard, XInput };

struct PadConfiguration {
    PadSource source{PadSource::Disabled};
    unsigned slot{};
    std::array<unsigned,8> keys{};
    std::array<unsigned,4> buttons{0x1000,0x2000,0x4000,0x8000}; // XInput A, B, X, Y.
};
using ControllerConfiguration=std::array<PadConfiguration,2>;

struct XInputButtonChoice { unsigned mask; const wchar_t* label; };
extern const std::array<XInputButtonChoice,8> xinputButtons;
ControllerConfiguration defaultControllerConfiguration();
bool assignablePadKey(unsigned key);
const wchar_t* validateControllerConfiguration(const ControllerConfiguration& configuration);

struct GamepadState {
    PadState pad;
    unsigned buttons{};
    bool connected{};
};

class InputManager final {
public:
    InputAction keyDown(unsigned virtualKey, bool repeat = false);
    void keyUp(unsigned virtualKey);
    void clear();
    void configure(const ControllerConfiguration& configuration);
    const ControllerConfiguration& configuration() const {return configuration_;}
    InputAction updateGamepad(unsigned player, GamepadState state, bool active);
    PadState pad(unsigned player = 0) const;
    unsigned rapidButtons(unsigned player) const;
private:
    ControllerConfiguration configuration_{defaultControllerConfiguration()};
    std::array<bool,256> keysDown_{};
    std::array<GamepadState,2> gamepads_{};
    std::array<bool,2> blocked_{true,true};
};
} // namespace smsgg::win32
