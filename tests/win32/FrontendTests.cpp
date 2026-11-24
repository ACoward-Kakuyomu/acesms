#include "win32/InputManager.h"
#include "win32/Viewport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>

#include <iostream>
#include <string_view>

namespace {
unsigned mask(smsgg::PadState pad) {
    return static_cast<unsigned>(pad.up) | (pad.down << 1) | (pad.left << 2)
        | (pad.right << 3) | (pad.button1 << 4) | (pad.button2 << 5);
}
bool keys() {
    smsgg::win32::InputManager input;
    constexpr unsigned keys[]{VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X'};
    for (unsigned combination = 0; combination < 64; ++combination) {
        input.clear();
        for (unsigned bit = 0; bit < 6; ++bit) if (combination & (1u << bit))
            if (input.keyDown(keys[bit]) != smsgg::win32::InputAction::None) return false;
        if (mask(input.pad()) != combination) return false;
        for (unsigned bit = 0; bit < 6; ++bit) input.keyUp(keys[bit]);
        if (mask(input.pad())) return false;
    }
    input.keyDown(VK_UP);
    input.keyDown(VK_UP);
    input.keyDown(VK_F1);
    input.keyUp(VK_F1);
    if (mask(input.pad()) != 1) return false;
    input.clear();
    input.keyDown('A');input.keyDown('S');
    if(mask(input.pad()) || input.rapidButtons(0)!=3 || input.rapidButtons(1)!=0) return false;
    input.keyUp('A');
    return input.rapidButtons(0)==2;
}
bool edges() {
    using smsgg::win32::InputAction;
    smsgg::win32::InputManager input;
    struct Edge { unsigned key; InputAction action; };
    constexpr Edge edges[]{
        {VK_RETURN, InputAction::ConsolePause},
        {VK_SPACE, InputAction::TogglePause},
        {VK_F2, InputAction::ToggleFastForward}
    };
    for (const auto [key, action] : edges) {
        if (input.keyDown(key) != action || input.keyDown(key) != InputAction::None
            || mask(input.pad())) return false;
        input.keyUp(key);
        if (input.keyDown(key) != action) return false;
        input.clear(); // Focus loss makes the next press a fresh edge.
        if (input.keyDown(key, true) != InputAction::None) return false;
        input.keyUp(key);
        if (input.keyDown(key) != action) return false;
        input.keyUp(key);
    }
    return input.keyDown(VK_F1) == InputAction::None;
}
bool gamepadMerge() {
    using namespace smsgg::win32;
    InputManager input;
    auto configuration=defaultControllerConfiguration();
    configuration[1].source=PadSource::XInput;
    configuration[1].slot=2;
    configuration[1].buttons={XINPUT_GAMEPAD_Y,XINPUT_GAMEPAD_LEFT_SHOULDER};
    if(validateControllerConfiguration(configuration)) return false;
    input.configure(configuration);
    input.keyDown('Z');
    input.updateGamepad(1,{{},0,true},true);
    input.updateGamepad(1,{{.right=true},XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_A,true},true);
    if(mask(input.pad(0))!=16 || mask(input.pad(1))!=24) return false;
    input.keyUp('Z');
    if(mask(input.pad(0)) || mask(input.pad(1))!=24) return false;
    input.updateGamepad(1,{},true);
    if(mask(input.pad(1))) return false;
    input.updateGamepad(1,{{},0,true},true);
    input.updateGamepad(1,{{.left=true},XINPUT_GAMEPAD_LEFT_SHOULDER,true},true);
    if(mask(input.pad(1))!=36) return false;
    input.clear();
    input.updateGamepad(1,{{.left=true},XINPUT_GAMEPAD_LEFT_SHOULDER,true},true);
    if(mask(input.pad(1))) return false;
    input.updateGamepad(1,{{},0,true},true);
    input.updateGamepad(1,{{.left=true},XINPUT_GAMEPAD_LEFT_SHOULDER,true},true);
    if(mask(input.pad(1))!=36) return false;

    configuration[1].source=PadSource::Keyboard;
    configuration[1].keys={'I','K','J','L','N','M'};
    if(validateControllerConfiguration(configuration)) return false;
    input.configure(configuration);
    input.keyDown('J');input.keyDown(VK_RIGHT);
    if(mask(input.pad(0))!=8 || mask(input.pad(1))!=4) return false;
    configuration[1].keys[0]='Z';
    if(!validateControllerConfiguration(configuration)) return false;
    configuration[1].keys[0]=VK_F5;
    if(!validateControllerConfiguration(configuration)) return false;
    configuration[1].keys[0]=0;
    if(!validateControllerConfiguration(configuration)) return false;

    configuration[0].source=configuration[1].source=PadSource::XInput;
    configuration[0].slot=configuration[1].slot;
    return validateControllerConfiguration(configuration)!=nullptr;
}
bool gamepadEdges() {
    using namespace smsgg::win32;
    auto config=defaultControllerConfiguration();
    config[0].source=PadSource::XInput;
    InputManager input;
    input.configure(config);
    GamepadState state{{},0,true};
    input.updateGamepad(0,state,true);
    state.buttons=XINPUT_GAMEPAD_START|XINPUT_GAMEPAD_BACK;
    if(input.updateGamepad(0,state,true)!=InputAction::TogglePause ||
        input.updateGamepad(0,state,true)!=InputAction::None) return false;
    state.buttons=0;input.updateGamepad(0,state,true);
    state.buttons=XINPUT_GAMEPAD_START;
    if(input.updateGamepad(0,state,true)!=InputAction::ConsolePause || !input.pad().start) return false;
    if(input.updateGamepad(0,state,false)!=InputAction::None || input.pad().start) return false;
    if(input.updateGamepad(0,state,true)!=InputAction::None || input.pad().start) return false;
    state.buttons=0;input.updateGamepad(0,state,true);
    state.buttons=XINPUT_GAMEPAD_START;
    if(input.updateGamepad(0,state,true)!=InputAction::ConsolePause || !input.pad().start) return false;
    if(input.updateGamepad(0,{},true)!=InputAction::None || input.pad().start) return false;
    constexpr std::array actions{InputAction::SaveState,InputAction::LoadState,InputAction::PreviousSlot,
        InputAction::NextSlot,InputAction::ToggleFastForward,InputAction::ToggleFullscreen};
    for(size_t i=0;i<actions.size();++i) {
        input.updateGamepad(0,{{},0,true},true);
        const GamepadState pressed{{},defaultShortcuts[i],true};
        if(input.updateGamepad(0,pressed,true)!=actions[i] ||
            input.updateGamepad(0,pressed,true)!=InputAction::None ||
            input.updateGamepad(1,pressed,true)!=InputAction::None) return false;
    }
    input.updateGamepad(0,{},true);
    if(input.updateGamepad(0,{{},leftTriggerButton,true},true)!=InputAction::None) return false;
    input.updateGamepad(0,{{},0,true},true);
    if(input.updateGamepad(0,{{},leftTriggerButton|rightTriggerButton,true},true)!=InputAction::None) return false;
    auto custom=defaultShortcuts;
    custom[0]=0;
    custom[1]=XINPUT_GAMEPAD_LEFT_SHOULDER;
    input.configure(config,custom);
    input.updateGamepad(0,{{},0,true},true);
    if(input.updateGamepad(0,{{},XINPUT_GAMEPAD_LEFT_SHOULDER,true},true)!=InputAction::LoadState) return false;
    return validateShortcuts(custom,config[0])==nullptr;
}
bool ggStart() {
    using namespace smsgg::win32;
    InputManager input;
    if(input.pad().start || input.keyDown(VK_RETURN)!=InputAction::ConsolePause || !input.pad().start) return false;
    if(input.keyDown(VK_RETURN,true)!=InputAction::None || !input.pad().start) return false;
    auto config=defaultControllerConfiguration();
    config[0].source=PadSource::XInput;
    input.configure(config);
    if(input.keyDown(VK_RETURN)!=InputAction::None || input.pad().start) return false;
    input.updateGamepad(0,{{},0,true},true);
    if(input.updateGamepad(0,{{},XINPUT_GAMEPAD_START,true},true)!=InputAction::ConsolePause || !input.pad().start)
        return false;
    input.clear();
    return !input.pad().start;
}
bool ggViewport() {
    struct Case {unsigned w,h; smsgg::win32::Viewport expected;};
    constexpr Case cases[]{ {0,0,{}},{0,144,{}},{160,0,{}},{1,1,{}},
        {160,144,{0,0,160,144}},{800,600,{80,12,640,576}},
        {319,287,{79,71,160,144}},{80,72,{0,0,80,72}},
        {120,100,{4,0,111,100}},{100,120,{0,15,100,90}} };
    for(const auto c:cases) {
        const auto v=smsgg::win32::calculateViewport(c.w,c.h,160,144);
        if(v.x!=c.expected.x || v.y!=c.expected.y || v.width!=c.expected.width || v.height!=c.expected.height) return false;
    }
    constexpr Case fourThreeCases[]{
        {0,144,{}},{3,2,{}},{800,600,{0,0,800,600}},
        {1000,601,{100,0,800,600}},{641,601,{0,60,640,480}},
        {120,100,{0,5,120,90}},{1920,1080,{240,0,1440,1080}}
    };
    for(const auto c:fourThreeCases) {
        const auto v=smsgg::win32::calculateViewport(c.w,c.h,160,144,smsgg::win32::AspectRatio::FourThree);
        if(v.x!=c.expected.x || v.y!=c.expected.y || v.width!=c.expected.width || v.height!=c.expected.height) return false;
    }
    return true;
}
bool viewport() {
    using namespace smsgg::win32;
    for(const auto aspect:{AspectRatio::FourThree,AspectRatio::SquarePixels}) {
        const auto missingWidth=calculateViewport(800,600,0,192,aspect);
        const auto missingHeight=calculateViewport(800,600,256,0,aspect);
        if(missingWidth.width || missingWidth.height || missingHeight.width || missingHeight.height) {
            std::cerr<<"Empty source produced a viewport\n";
            return false;
        }
    }
    struct Case { unsigned w, h; smsgg::win32::Viewport expected; };
    constexpr Case cases[]{
        {0, 0, {}}, {0, 192, {}}, {256, 0, {}}, {1, 1, {}},
        {256, 192, {0, 0, 256, 192}}, {512, 384, {0, 0, 512, 384}},
        {800, 600, {16, 12, 768, 576}}, {700, 500, {94, 58, 512, 384}},
        {257, 193, {0, 0, 256, 192}}, {128, 96, {0, 0, 128, 96}},
        {256, 191, {1, 0, 254, 191}}, {255, 192, {0, 0, 255, 191}},
        {100, 192, {0, 58, 100, 75}}, {256, 100, {61, 0, 133, 100}},
        {2, 2, {0, 0, 2, 1}}, {2048, 1536, {0, 0, 2048, 1536}} };
    for (const auto test : cases) {
        const auto actual = smsgg::win32::calculateViewport(test.w, test.h);
        if (actual.x != test.expected.x || actual.y != test.expected.y
            || actual.width != test.expected.width || actual.height != test.expected.height) {
            std::cerr << "Viewport mismatch: " << test.w << 'x' << test.h << '\n';
            return false;
        }
    }
    return true;
}
}
int main(int argc, char** argv) {
    const std::string_view name = argc == 2 ? argv[1] : "";
    const bool pass = name == "keys" ? keys() : name == "edges" ? edges() : name == "viewport" ? viewport()
        : name == "gamepad_merge" ? gamepadMerge() : name == "gg_start" ? ggStart()
        : name == "gg_viewport" ? ggViewport() : name == "gamepad_edges" && gamepadEdges();
    if (!pass) std::cerr << "Frontend test failed: " << name << '\n';
    return pass ? 0 : 1;
}
