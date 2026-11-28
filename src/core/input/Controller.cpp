#include "core/input/Controller.h"

namespace smsgg {
bool Controller::pinLevel(unsigned pin, bool input) const {
    return control_ & (1u << pin) ? input : (control_ & (0x10u << pin)) != 0;
}

uint8_t Controller::thLevels() const {
    return static_cast<uint8_t>((pinLevel(1, true) ? 0x40 : 0) | (pinLevel(3, true) ? 0x80 : 0));
}

bool Controller::writeControl(uint8_t value) {
    const auto previous = thLevels();
    control_ = value;
    return (thLevels() & ~previous) != 0;
}

uint8_t Controller::readDc(bool smsControl) const {
    const unsigned pressed = (digitalPadBits(state_) & 0x1f) | ((digitalPadBits(player2_) & 3) << 6);
    const bool tr=smsControl?pinLevel(0,!state_.button2):!state_.button2;
    return static_cast<uint8_t>(((~pressed) & 0xdf)
        | (tr ? 0x20 : 0));
}

uint8_t Controller::readDd(bool smsControl) const {
    const unsigned pressed = digitalPadBits(player2_) >> 2;
    if(!smsControl) return static_cast<uint8_t>(0xff ^ pressed);
    return static_cast<uint8_t>((0x37 & ~pressed) | (pinLevel(2, !player2_.button2) ? 8 : 0)
        | thLevels());
}
} // namespace smsgg
