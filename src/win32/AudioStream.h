#pragma once

#include "win32/PcmRing.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <mutex>
#include <string>

namespace smsgg::win32 {
enum class AudioCommand { Stopped, Playing };
struct AudioControl {
    AudioCommand command{AudioCommand::Stopped};
    uint64_t sequence{};
    bool muted{},shutdown{};
};
struct AudioStatus {
    uint64_t acknowledged{},consumed{},silence{};
    uint32_t padding{}; // Last observed native padding before refill, not a synchronized queue snapshot.
    bool playing{},exited{};
    std::string error;
};
class AudioStream final {
    const uint32_t rate_,buffer_;
    const size_t target_;
public:
    AudioStream(uint32_t sampleRate,uint32_t bufferFrames);
    PcmRing pcm;
    uint32_t sampleRate() const {return rate_;}
    uint32_t bufferFrames() const {return buffer_;}
    uint32_t availableRenderFrames(uint32_t padding) const;
    size_t targetFrames() const {return target_;}
    HANDLE producerEvent() const {return producer_.get();}
    HANDLE consumerEvent() const {return consumer_.get();}
    void wakeProducer();
    void wakeConsumer();
    uint64_t request(AudioCommand command,bool muted);
    AudioControl control() const;
    AudioStatus status() const;
    void acknowledge(uint64_t sequence,bool playing);
    void recordRead(uint64_t consumed,uint64_t silence,uint32_t padding);
    void fail(std::string error);
    void markExited();
    void shutdown(); // Owner only, after the producer has joined.
private:
    using Handle=std::unique_ptr<void,decltype(&::CloseHandle)>;
    static Handle createEvent();
    Handle producer_,consumer_;
    mutable std::mutex mutex_;
    AudioControl control_;
    AudioStatus status_;
};
} // namespace smsgg::win32
