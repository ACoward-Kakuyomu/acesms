#pragma once

#include "core/audio/AudioClock.h"
#include "core/audio/AudioFrame.h"
#include "core/audio/Psg.h"
#include "core/audio/FmBackend.h"
#include "core/audio/FmResampler.h"

#include <optional>
#include <span>
#include <vector>

namespace smsgg {
struct FmAudioState {
    std::vector<uint8_t> backend;
    FmResamplerState resampler;
};
struct AudioMixerState {
    PsgState psg;
    AudioClockState clock;
    uint64_t time{},interval{};
    PsgArea area;
    uint8_t fmControl{};
    std::optional<FmAudioState> fm;
};
class AudioMixer final {
public:
    AudioMixer(uint32_t clockHz,uint32_t sampleRate,bool fmEnabled = false,FmInterpolation interpolation=FmInterpolation::Sinc,
        FmBackendPtr backend={},Machine machine=Machine::Sms);
    bool hasFm() const noexcept { return fm_.has_value(); }
    void writePsg(uint8_t value) { psg_.write(value); }
    void writeStereo(uint8_t mask) { psg_.writeStereo(mask); }
    void writeFm(uint8_t addressOrData,uint8_t value) { fm_->chip.writeIo(addressOrData,value); }
    void writeFmControl(uint8_t value) { fmControl_=value&3; }
    uint8_t readFmControl() const { return fmControl_&1; }
    size_t advanceTo(uint64_t target,std::span<AudioFrame> output);
    AudioMixerState captureState() const;
    // Same validated profile. Opaque backend load is the only fallible restore step.
    acesms_fm_result restoreState(const AudioMixerState& state);
private:
    Psg psg_;
    AudioClock clock_;
    uint64_t time_{}, interval_{};
    PsgArea area_{};
    struct FmVoice {
        FmVoice(uint32_t clock,uint32_t rate,FmInterpolation interpolation,FmBackendPtr backend)
            : chip(backend?std::move(backend):FmBackendDefinition::builtin(),clock),converter(clock,rate,interpolation) {}
        FmBackend chip;
        FmResampler converter;
    };
    std::optional<FmVoice> fm_;
    uint8_t fmControl_{};
};
} // namespace smsgg
