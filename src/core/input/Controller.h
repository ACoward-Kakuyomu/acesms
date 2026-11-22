#pragma once

#include <cstdint>

namespace smsgg {
struct PadState {
    bool up{}, down{}, left{}, right{}, button1{}, button2{}, start{};
};

constexpr uint8_t digitalPadBits(PadState pad) {
    return static_cast<uint8_t>(static_cast<unsigned>(pad.up) | (pad.down << 1) | (pad.left << 2) | (pad.right << 3)
        | (pad.button1 << 4) | (pad.button2 << 5));
}

struct ControllerState {
    PadState pad;
    uint8_t control{0xff};
    PadState player2;
};

// Digital pads and the console's Start level, with SMS port-A/B pin control.
class Controller final {
public:
    void setState(PadState state, PadState player2 = {}) { state_ = state; player2_ = player2; }
    void writeControl(uint8_t value) { control_ = value; }
    uint8_t readDc(bool smsControl = true) const;
    uint8_t readDd(bool smsControl = true) const;
    bool startPressed() const { return state_.start; }
    ControllerState captureState() const noexcept { return {state_,control_,player2_}; }
    void restoreState(const ControllerState& state) noexcept { setState(state.pad,state.player2);control_=state.control; }
private:
    bool pinLevel(unsigned pin, bool input) const;
    PadState state_{};
    PadState player2_{};
    uint8_t control_{0xff};
};
} // namespace smsgg
