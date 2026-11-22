#pragma once

#include "core/audio/AudioFrame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>

namespace smsgg::win32 {
class AudioFormat final {
public:
    static std::variant<AudioFormat,std::string> parse(std::span<const std::byte> bytes);
    void write(std::span<const AudioFrame> frames,std::span<std::byte> output,bool mute) const;
    uint32_t sampleRate() const {return rate_;}
    uint16_t channels() const {return channels_;}
    uint16_t blockAlign() const {return block_;}
private:
    AudioFormat()=default;
    uint32_t rate_{};
    uint16_t channels_{},block_{},bits_{},valid_{};
    bool floating_{};
};
} // namespace smsgg::win32
