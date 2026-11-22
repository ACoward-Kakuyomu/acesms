#pragma once

#include <cstdint>

namespace smsgg {
struct AudioClockState { uint64_t phase{}; };
// Rates are validated at the external configuration boundary: 0 < rate <= clock.
class AudioClock final {
public:
    AudioClock(uint32_t clockHz, uint32_t sampleRate) : clock_(clockHz), rate_(sampleRate) {}
    AudioClockState captureState() const noexcept { return {phase_}; }
    // Same validated clock/rate; the external state boundary validates phase.
    void restoreState(const AudioClockState& state) noexcept { phase_=state.phase; }
    uint32_t tstatesToSample() const { return static_cast<uint32_t>((clock_-phase_+rate_-1)/rate_); }
    uint64_t advance(uint64_t elapsed) {
        const auto partial=phase_+(elapsed%clock_)*rate_;
        const auto frames=(elapsed/clock_)*rate_+partial/clock_;
        phase_=partial%clock_;
        return frames;
    }
private:
    uint32_t clock_,rate_;
    uint64_t phase_{};
};
} // namespace smsgg
