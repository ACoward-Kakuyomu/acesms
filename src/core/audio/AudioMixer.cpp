#include "core/audio/AudioMixer.h"

#include <algorithm>
#include <cassert>

namespace smsgg {
namespace {
constexpr float psgGain=.5f, fmGain=.5f;
}
AudioMixer::AudioMixer(uint32_t clockHz,uint32_t sampleRate,bool fmEnabled,FmInterpolation interpolation,FmBackendPtr backend,Machine machine)
    : psg_(machine), clock_(clockHz,sampleRate) {
    if(fmEnabled) fm_.emplace(clockHz,sampleRate,interpolation,std::move(backend));
}
AudioMixerState AudioMixer::captureState() const {
    AudioMixerState state{psg_.captureState(),clock_.captureState(),time_,interval_,area_,fmControl_,{}};
    if(fm_) state.fm=FmAudioState{fm_->chip.saveState(),fm_->converter.captureState()};
    return state;
}
acesms_fm_result AudioMixer::restoreState(const AudioMixerState& state) {
    assert(fm_.has_value()==state.fm.has_value());
    if(fm_) {
        const auto result=fm_->chip.loadState(state.fm->backend,state.time);
        if(result!=ACESMS_FM_OK) return result;
        fm_->converter.restoreState(state.fm->resampler);
    }
    psg_.restoreState(state.psg);clock_.restoreState(state.clock);
    time_=state.time;interval_=state.interval;area_=state.area;fmControl_=state.fmControl;
    return ACESMS_FM_OK;
}
size_t AudioMixer::advanceTo(uint64_t target,std::span<AudioFrame> output) {
    assert(target>=time_);
    size_t count=0;
    while(time_<target) {
        auto elapsed=std::min<uint64_t>(target-time_,clock_.tstatesToSample());
        if(fm_) elapsed=std::min<uint64_t>(elapsed,72-time_%72);
        const auto area=psg_.advance(elapsed);
        if(!fm_ || fmControl_==0 || fmControl_==3) {
            area_.left+=area.left;area_.right+=area.right;
        }
        interval_+=elapsed;
        time_+=elapsed;
        if(fm_) {
            std::array<int32_t,1> native{};
            if(fm_->chip.advanceTo(time_,native)) fm_->converter.push(native[0]);
        }
        if(clock_.advance(elapsed)) {
            assert(count<output.size());
            AudioFrame frame{static_cast<float>(area_.left/static_cast<double>(interval_)),
                static_cast<float>(area_.right/static_cast<double>(interval_))};
            if(fm_) {
                const auto sample=fm_->converter.sample(); // Gate/Mute never stop chip or conversion clocks.
                const float fm=(fmControl_&1) ? static_cast<float>(sample/32768*fmGain) : 0;
                frame={frame.left*psgGain+fm,frame.right*psgGain+fm};
            }
            output[count++]=frame;
            area_={};
            interval_=0;
        }
    }
    return count;
}
} // namespace smsgg
