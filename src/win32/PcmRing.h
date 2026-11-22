#pragma once

#include "core/audio/AudioFrame.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace smsgg::win32 {
// One producer and one consumer. reset requires both endpoints to be quiescent.
class PcmRing final {
public:
    explicit PcmRing(size_t capacity) : data_(capacity) {
        assert(capacity>=2 && std::has_single_bit(capacity));
    }
    size_t queued() const { // Producer only: its own write index cannot be stale.
        return static_cast<size_t>(write_.load(std::memory_order_relaxed)-read_.load(std::memory_order_acquire));
    }
    size_t push(std::span<const AudioFrame> input) {
        const auto write=write_.load(std::memory_order_relaxed);
        const auto read=read_.load(std::memory_order_acquire);
        const auto n=std::min(input.size(),data_.size()-static_cast<size_t>(write-read));
        const auto at=static_cast<size_t>(write)&(data_.size()-1);
        const auto first=std::min(n,data_.size()-at);
        std::copy_n(input.begin(),first,data_.begin()+at);
        std::copy_n(input.begin()+first,n-first,data_.begin());
        write_.store(write+n,std::memory_order_release);
        return n;
    }
    size_t pop(std::span<AudioFrame> output) {
        const auto read=read_.load(std::memory_order_relaxed);
        const auto write=write_.load(std::memory_order_acquire);
        const auto n=std::min(output.size(),static_cast<size_t>(write-read));
        const auto at=static_cast<size_t>(read)&(data_.size()-1);
        const auto first=std::min(n,data_.size()-at);
        std::copy_n(data_.begin()+at,first,output.begin());
        std::copy_n(data_.begin(),n-first,output.begin()+first);
        read_.store(read+n,std::memory_order_release);
        return n;
    }
    void reset() {
        read_.store(0,std::memory_order_relaxed);
        write_.store(0,std::memory_order_relaxed);
    }
private:
    std::vector<AudioFrame> data_;
    std::atomic<uint64_t> read_{},write_{};
};
} // namespace smsgg::win32
