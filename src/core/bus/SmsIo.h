#pragma once

#include "core/cpu/Z80.h"
#include "core/input/Controller.h"
#include "core/audio/AudioMixer.h"
#include "core/Machine.h"

namespace smsgg {
// Terminal device after IoBus, MemoryControl and VdpIo have handled their ports.
class SmsIo final : public Z80Io {
public:
    SmsIo(Controller& controller, AudioMixer& audio, Machine machine = Machine::Sms, bool fmEnabled = false)
        : controller_(controller), audio_(audio), machine_(machine), fmEnabled_(fmEnabled) {}
    uint8_t readPort(uint16_t port) override;
    void writePort(uint16_t port, uint8_t value, uint64_t tstate = 0) override;
private:
    Controller& controller_;
    AudioMixer& audio_;
    Machine machine_;
    bool fmEnabled_;
};
} // namespace smsgg
