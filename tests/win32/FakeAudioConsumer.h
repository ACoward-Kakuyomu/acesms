#pragma once

#include "win32/AudioStream.h"

#include <array>
#include <atomic>
#include <thread>

namespace smsgg::test {
// Test-owned consumer. No host clock paces production; tests grant exact frame counts.
class FakeAudioConsumer final {
public:
    win32::AudioStream stream;
    explicit FakeAudioConsumer(bool automatic=true,uint32_t rate=48000)
        : stream(rate,480),automatic_(automatic),thread_([this]{run();}) {}
    ~FakeAudioConsumer() {stream.shutdown();thread_.join();}
    void grant(size_t frames) {budget_.fetch_add(frames);stream.wakeConsumer();}
    void holdStop(bool hold) {holdStop_.store(hold);stream.wakeConsumer();}
private:
    void run() {
        std::array<AudioFrame,480> frames{};
        for(;;) {
            const auto control=stream.control();
            if(control.shutdown) break;
            if(control.command==win32::AudioCommand::Stopped) {
                if(!holdStop_.load() && stream.status().acknowledged!=control.sequence)
                    stream.acknowledge(control.sequence,false);
            } else {
                if(stream.status().acknowledged!=control.sequence) stream.acknowledge(control.sequence,true);
                size_t remaining=automatic_?frames.size():budget_.exchange(0);
                while(remaining) {
                    const auto n=stream.pcm.pop(std::span(frames).first(std::min(remaining,frames.size())));
                    if(!n) break;
                    remaining-=n;stream.recordRead(n,0,0);
                }
            }
            if(WaitForSingleObject(stream.consumerEvent(),INFINITE)!=WAIT_OBJECT_0) break;
        }
        stream.markExited();
    }
    bool automatic_;
    std::atomic<size_t> budget_{};
    std::atomic<bool> holdStop_{};
    std::thread thread_;
};
}
