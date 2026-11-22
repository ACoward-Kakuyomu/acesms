#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace smsgg {
enum class FmInterpolation : uint8_t { Sinc, ZohLowPass, Linear };
struct FmResamplerState {
    uint64_t nativeIndex{},outputIndex{},position{},phase{};
    std::array<double,33> history{};
    uint8_t next{};
    double delay1{},delay2{};
};
// Internal configuration is validated once at the session boundary.
class FmResampler final {
public:
    FmResampler(uint32_t masterClock,uint32_t outputRate,FmInterpolation method=FmInterpolation::Sinc);
    void push(int32_t nativeSample);
    // Caller has pushed native samples through this PCM notification's integer T-state.
    double sample();
    void reset() noexcept { state_={}; }
    FmResamplerState captureState() const noexcept { return state_; }
    void restoreState(const FmResamplerState& state) noexcept { state_=state; }
    static size_t stateBytes(FmInterpolation method) noexcept;
    static void encodeState(const FmResamplerState& state,FmInterpolation method,std::span<uint8_t> output);
    // External bytes, interpreted under an already validated FM profile and saved System time.
    static std::optional<FmResamplerState> decodeState(std::span<const uint8_t> bytes,uint32_t clock,uint32_t rate,FmInterpolation method,uint64_t masterTicks);
private:
    double value(uint64_t index) const;
    double filter(double input);
    uint32_t clock_,rate_;
    FmInterpolation method_;
    double cutoff_{};
    std::array<double,3> b_{};
    std::array<double,2> a_{};
    FmResamplerState state_{};
};
} // namespace smsgg
