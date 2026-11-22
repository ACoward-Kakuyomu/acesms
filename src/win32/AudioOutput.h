#pragma once

#include "win32/AudioStream.h"

#include <future>
#include <thread>

namespace smsgg::win32 {
class AudioOutput final {
public:
    AudioOutput();
    ~AudioOutput();
    AudioStream& stream() const {return *stream_;}
    const std::string& description() const {return description_;}
    void stop();
private:
    void run(std::promise<void> initialized);
    std::unique_ptr<AudioStream> stream_;
    std::string description_;
    std::thread thread_;
};
} // namespace smsgg::win32
