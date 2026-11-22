#include "core/audio/Psg.h"

#include <algorithm>

namespace smsgg {
Psg::Psg(Machine machine)
    : lfsr_(machine == Machine::Sg1000 ? 0x4000 : 0x8000), sg1000_(machine == Machine::Sg1000) {}

void Psg::write(uint8_t value) {
    if (value & 0x80) selected_ = (value >> 4) & 7;
    const auto channel = selected_ >> 1;
    if (selected_ & 1) volumes_[channel] = value & 15;
    else if (selected_ == 6) {
        noise_ = value & 7;
        lfsr_ = sg1000_ ? 0x4000 : 0x8000;
        noiseOutput_ = noisePhase_ = false;
        noiseCounter_ = static_cast<uint16_t>(16u << (noise_ & 3));
    } else if (value & 0x80) tones_[channel] = static_cast<uint16_t>((tones_[channel] & 0x3f0) | (value & 15));
    else tones_[channel] = static_cast<uint16_t>((tones_[channel] & 15) | ((value & 63) << 4));
}

PsgArea Psg::advance(uint64_t elapsed) {
    constexpr std::array<double,16> amplitudes{1, .7943282347242815, .6309573444801932,
        .5011872336272722, .3981071705534972, .3162277660168379, .2511886431509580,
        .1995262314968879, .1584893192461113, .1258925411794167, .1,
        .0794328234724282, .0630957344480193, .0501187233627272, .0398107170553497, 0};
    PsgArea area;
    while (elapsed) {
        const auto chunk = std::min<uint64_t>(elapsed, 16-divider_);
        const double noiseLevel = (noiseOutput_ ? 1 : -1) * amplitudes[volumes_[3]];
        double left = (stereo_ & 0x80) ? noiseLevel : 0;
        double right = (stereo_ & 0x08) ? noiseLevel : 0;
        for (unsigned ch=0; ch<3; ++ch) {
            const bool high = outputs_[ch] || (!sg1000_ && tones_[ch] < 2);
            const auto level = (high ? 1 : -1) * amplitudes[volumes_[ch]];
            if (stereo_ & (0x10 << ch)) left += level;
            if (stereo_ & (1 << ch)) right += level;
        }
        area.left += left * .25 * static_cast<double>(chunk);
        area.right += right * .25 * static_cast<double>(chunk);
        elapsed -= chunk;
        divider_ += static_cast<unsigned>(chunk);
        if (divider_ != 16) continue;
        divider_ = 0;
        const bool oldTone2 = outputs_[2];
        for (unsigned ch=0; ch<3; ++ch) {
            if (--counters_[ch] == 0) {
                counters_[ch] = tones_[ch] == 0 ? (sg1000_ ? 1024 : 1) : tones_[ch];
                outputs_[ch] = !outputs_[ch];
            }
        }
        bool shift = !oldTone2 && outputs_[2];
        if ((noise_ & 3) != 3) {
            shift = false;
            if (--noiseCounter_ == 0) {
                noiseCounter_ = static_cast<uint16_t>(16u << (noise_ & 3));
                noisePhase_ = !noisePhase_;
                shift = noisePhase_;
            }
        }
        if (shift) {
            noiseOutput_ = (lfsr_ & 1) != 0;
            const auto feedback = (noise_ & 4) ? ((lfsr_ ^ (lfsr_ >> (sg1000_ ? 1 : 3))) & 1) : (lfsr_ & 1);
            lfsr_ = static_cast<uint16_t>((lfsr_ >> 1) | (feedback << (sg1000_ ? 14 : 15)));
        }
    }
    return area;
}
} // namespace smsgg
