#include "win32/AudioStream.h"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace smsgg::win32 {
namespace {
void signal(HANDLE event) {
    if(!SetEvent(event)) throw std::system_error(GetLastError(),std::system_category(),"Set audio event");
}
}
AudioStream::AudioStream(uint32_t rate,uint32_t buffer)
    : rate_(rate),buffer_(buffer),target_(std::max<size_t>(buffer,(uint64_t(rate)*40+999)/1000)),
      pcm(std::bit_ceil(std::max<size_t>(2,target_))),
      producer_(createEvent()),consumer_(createEvent()) {}
uint32_t AudioStream::availableRenderFrames(uint32_t padding) const {
    if(padding>buffer_) throw std::runtime_error("GetCurrentPadding exceeds the WASAPI buffer size");
    return buffer_-padding;
}
AudioStream::Handle AudioStream::createEvent() {
    Handle event(CreateEventW(nullptr,FALSE,FALSE,nullptr),&::CloseHandle);
    if(!event) throw std::system_error(GetLastError(),std::system_category(),"Create audio event");
    return event;
}
void AudioStream::wakeProducer() {signal(producer_.get());}
void AudioStream::wakeConsumer() {signal(consumer_.get());}
uint64_t AudioStream::request(AudioCommand command,bool muted) {
    uint64_t sequence;
    {
        std::lock_guard lock(mutex_);
        if(control_.command==command && control_.muted==muted) return control_.sequence;
        control_.command=command;control_.muted=muted;
        sequence=++control_.sequence;
    }
    wakeConsumer();
    return sequence;
}
AudioControl AudioStream::control() const {std::lock_guard lock(mutex_);return control_;}
AudioStatus AudioStream::status() const {std::lock_guard lock(mutex_);return status_;}
void AudioStream::acknowledge(uint64_t sequence,bool playing) {
    {std::lock_guard lock(mutex_);status_.acknowledged=sequence;status_.playing=playing;}
    wakeProducer();
}
void AudioStream::recordRead(uint64_t consumed,uint64_t silence,uint32_t padding) {
    {std::lock_guard lock(mutex_);status_.consumed+=consumed;status_.silence+=silence;status_.padding=padding;}
    wakeProducer();
}
void AudioStream::fail(std::string error) {
    {std::lock_guard lock(mutex_);status_.error=std::move(error);status_.playing=false;}
    wakeProducer();
}
void AudioStream::markExited() {
    {std::lock_guard lock(mutex_);status_.exited=true;status_.playing=false;}
    wakeProducer();
}
void AudioStream::shutdown() {
    {std::lock_guard lock(mutex_);control_.shutdown=true;++control_.sequence;}
    wakeConsumer();
}
} // namespace smsgg::win32
