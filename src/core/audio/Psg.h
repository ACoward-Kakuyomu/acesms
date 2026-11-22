#pragma once

#include "core/Machine.h"

#include <array>
#include <cstdint>

namespace smsgg {
struct PsgArea { double left{}, right{}; };
struct PsgState {
    std::array<uint16_t,3> tones{};
    std::array<uint8_t,4> volumes{15,15,15,15};
    std::array<uint16_t,3> counters{1,1,1};
    std::array<bool,3> outputs{};
    uint8_t selected{},noise{},stereo{0xff},divider{};
    uint16_t noiseCounter{16},lfsr{0x8000};
    bool noisePhase{},noiseOutput{};
};
class Psg final {
public:
    explicit Psg(Machine machine = Machine::Sms);
    void write(uint8_t value);
    void writeStereo(uint8_t mask) { stereo_ = mask; }
    PsgArea advance(uint64_t elapsedTstates);
    PsgState captureState() const noexcept {
        return {tones_,volumes_,counters_,outputs_,selected_,noise_,stereo_,static_cast<uint8_t>(divider_),
            noiseCounter_,lfsr_,noisePhase_,noiseOutput_};
    }
    // Captured or externally validated values. No register writes/reset on restore.
    void restoreState(const PsgState& state) noexcept {
        tones_=state.tones;volumes_=state.volumes;counters_=state.counters;outputs_=state.outputs;
        selected_=state.selected;noise_=state.noise;stereo_=state.stereo;divider_=state.divider;
        noiseCounter_=state.noiseCounter;lfsr_=state.lfsr;noisePhase_=state.noisePhase;noiseOutput_=state.noiseOutput;
    }
private:
    friend struct PsgTestAccess;
    std::array<uint16_t,3> tones_{};
    std::array<uint8_t,4> volumes_{15,15,15,15};
    std::array<uint16_t,3> counters_{1,1,1};
    std::array<bool,3> outputs_{};
    uint8_t selected_{}, noise_{};
    uint8_t stereo_{0xff};
    unsigned divider_{};
    uint16_t noiseCounter_{16}, lfsr_{0x8000};
    bool noisePhase_{}, noiseOutput_{};
    const bool sg1000_;
};
} // namespace smsgg
