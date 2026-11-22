#include "win32/InputManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <utility>

namespace smsgg::win32 {
const std::array<XInputButtonChoice,8> xinputButtons{{
    {XINPUT_GAMEPAD_A,L"A"},{XINPUT_GAMEPAD_B,L"B"},{XINPUT_GAMEPAD_X,L"X"},{XINPUT_GAMEPAD_Y,L"Y"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER,L"Left shoulder"},{XINPUT_GAMEPAD_RIGHT_SHOULDER,L"Right shoulder"},
    {XINPUT_GAMEPAD_LEFT_THUMB,L"Left stick press"},{XINPUT_GAMEPAD_RIGHT_THUMB,L"Right stick press"}
}};

ControllerConfiguration defaultControllerConfiguration() {
    return {PadConfiguration{.source=PadSource::Keyboard,.keys={VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,'Z','X','A','S'}},
        PadConfiguration{}};
}

bool assignablePadKey(unsigned key) {
    if(key<VK_BACK || key>=0xf0 || (key>='0' && key<='9') || (key>=VK_F5 && key<=VK_F9)) return false;
    switch(key) {
    case VK_TAB: case VK_ESCAPE: case VK_RETURN: case VK_SPACE: case VK_F2:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
    case VK_PROCESSKEY: case VK_PACKET: return false;
    default: return true;
    }
}

const wchar_t* validateControllerConfiguration(const ControllerConfiguration& configuration) {
    if(configuration[0].source==PadSource::Disabled) return L"Player 1 must have an input source.";
    std::array<bool,256> usedKeys{};
    for(const auto& pad:configuration) {
        if(pad.slot>=XUSER_MAX_COUNT) return L"Select a valid XInput slot.";
        for(size_t i=0;i<pad.buttons.size();++i) {
            const auto button=pad.buttons[i];
            if(i>=2 && !button) continue;
            if(std::ranges::find(xinputButtons,button,&XInputButtonChoice::mask)==xinputButtons.end())
                return L"Select valid XInput buttons.";
        }
        switch(pad.source) {
        case PadSource::Disabled:
        case PadSource::XInput: break;
        case PadSource::Keyboard:
            for(size_t i=0;i<pad.keys.size();++i) {
                const auto key=pad.keys[i];
                if(i>=6 && !key) continue;
                if(!key) return L"Assign all six keyboard controls for each keyboard player.";
                if(!assignablePadKey(key)) return L"A keyboard binding uses a reserved key.";
                if(std::exchange(usedKeys[key],true)) return L"A keyboard key is assigned more than once.";
            }
            break;
        default: return L"Select a valid input source.";
        }
    }
    if(configuration[0].source==PadSource::XInput && configuration[1].source==PadSource::XInput &&
        configuration[0].slot==configuration[1].slot) return L"Each XInput player needs a different slot.";
    return nullptr;
}

InputAction InputManager::keyDown(unsigned key, bool repeat) {
    if(key>=keysDown_.size() || repeat || std::exchange(keysDown_[key],true)) return InputAction::None;
    if(key==VK_RETURN && configuration_[0].source==PadSource::Keyboard) return InputAction::ConsolePause;
    if(key==VK_SPACE) return InputAction::TogglePause;
    if(key==VK_F2) return InputAction::ToggleFastForward;
    return InputAction::None;
}

void InputManager::keyUp(unsigned key) {
    if(key<keysDown_.size()) keysDown_[key]=false;
}

void InputManager::clear() {
    keysDown_={};
    gamepads_={};
    blocked_.fill(true);
}

void InputManager::configure(const ControllerConfiguration& configuration) {
    configuration_=configuration;
    clear();
}

InputAction InputManager::updateGamepad(unsigned player, GamepadState state, bool active) {
    auto& current=gamepads_[player];
    auto& blocked=blocked_[player];
    if(!active || !state.connected) {
        current={};
        blocked=true;
        return InputAction::None;
    }
    if(blocked) {
        if(state.buttons || digitalPadBits(state.pad)) return InputAction::None;
        blocked=false;
    }
    const auto pressed=state.buttons & ~current.buttons;
    current=state;
    if(player==0) {
        if(pressed & XINPUT_GAMEPAD_BACK) return InputAction::TogglePause;
        if(pressed & XINPUT_GAMEPAD_START) return InputAction::ConsolePause;
    }
    return InputAction::None;
}

PadState InputManager::pad(unsigned player) const {
    const auto& config=configuration_[player];
    switch(config.source) {
    case PadSource::Keyboard: {
        const auto& keys=config.keys;
        return {keysDown_[keys[0]],keysDown_[keys[1]],keysDown_[keys[2]],keysDown_[keys[3]],
            keysDown_[keys[4]],keysDown_[keys[5]],player==0 && keysDown_[VK_RETURN]};
    }
    case PadSource::XInput: {
        const auto& gamepad=gamepads_[player];
        auto pad=gamepad.pad;
        pad.button1=(gamepad.buttons & config.buttons[0])!=0;
        pad.button2=(gamepad.buttons & config.buttons[1])!=0;
        pad.start=player==0 && (gamepad.buttons & XINPUT_GAMEPAD_START)!=0;
        return pad;
    }
    case PadSource::Disabled: return {};
    }
    return {};
}

unsigned InputManager::rapidButtons(unsigned player) const {
    const auto& config=configuration_[player];
    switch(config.source) {
    case PadSource::Keyboard:
        return static_cast<unsigned>(keysDown_[config.keys[6]]) | (keysDown_[config.keys[7]]<<1);
    case PadSource::XInput: {
        const auto buttons=gamepads_[player].buttons;
        return static_cast<unsigned>((buttons&config.buttons[2])!=0) | (((buttons&config.buttons[3])!=0)<<1);
    }
    case PadSource::Disabled: return 0;
    }
    return 0;
}
} // namespace smsgg::win32
