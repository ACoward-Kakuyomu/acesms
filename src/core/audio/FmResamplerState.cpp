#include "core/audio/FmResampler.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

namespace smsgg {
namespace {
size_t historyCount(FmInterpolation method) {
    switch(method) {
    case FmInterpolation::Sinc:return 33;
    case FmInterpolation::ZohLowPass:return 2;
    case FmInterpolation::Linear:return 3;
    }
    return 0;
}
void put(std::span<uint8_t> out,size_t& at,uint64_t value) {
    for(unsigned n=0;n<8;++n) {out[at++]=static_cast<uint8_t>(value);value>>=8;}
}
uint64_t get(std::span<const uint8_t> bytes,size_t& at) {
    uint64_t value=0;
    for(unsigned n=0;n<8;++n) value|=uint64_t(bytes[at++])<<(n*8);
    return value;
}
}
size_t FmResampler::stateBytes(FmInterpolation method) noexcept {
    const auto count=historyCount(method);
    return count?32+count*8+(method==FmInterpolation::ZohLowPass?16:0):0;
}
void FmResampler::encodeState(const FmResamplerState& state,FmInterpolation method,std::span<uint8_t> output) {
    assert(stateBytes(method) && output.size()>=stateBytes(method));
    size_t at=0;
    put(output,at,state.nativeIndex);put(output,at,state.outputIndex);put(output,at,state.position);put(output,at,state.phase);
    for(size_t age=0;age<historyCount(method);++age) put(output,at,std::bit_cast<uint64_t>(state.history[(state.next+32-age)%33]));
    if(method==FmInterpolation::ZohLowPass) {put(output,at,std::bit_cast<uint64_t>(state.delay1));put(output,at,std::bit_cast<uint64_t>(state.delay2));}
}
std::optional<FmResamplerState> FmResampler::decodeState(std::span<const uint8_t> bytes,uint32_t clock,uint32_t rate,FmInterpolation method,uint64_t masterTicks) {
    const auto count=historyCount(method);
    if(!count || bytes.size()!=stateBytes(method)) return std::nullopt;
    size_t at=0;FmResamplerState state;
    state.nativeIndex=get(bytes,at);state.outputIndex=get(bytes,at);state.position=get(bytes,at);state.phase=get(bytes,at);
    const uint64_t denominator=uint64_t(rate)*72;
    const uint64_t clockPhase=((masterTicks%clock)*rate)%clock;
    const uint64_t remainder=(masterTicks%72)*rate;
    const uint64_t delta=clockPhase/denominator+(clockPhase%denominator>remainder?1:0);
    if(state.nativeIndex!=masterTicks/72 || state.outputIndex!=masterTicks/clock*rate+(masterTicks%clock)*rate/clock ||
        delta>state.nativeIndex || state.position!=state.nativeIndex-delta || state.phase!=(remainder+denominator-clockPhase%denominator)%denominator) return std::nullopt;
    state.next=static_cast<uint8_t>(state.nativeIndex%33);
    double historyBound=2147483648.0,delay1Bound=0,delay2Bound=0;
    const bool filteredHistory=method==FmInterpolation::ZohLowPass && uint64_t(rate)*72<clock;
    if(method==FmInterpolation::ZohLowPass) {
        const FmResampler converter(clock,rate,method);
        const auto& a=converter.a_;const auto& b=converter.b_;
        // h[1+k] = r^k * (h1*cos(k*theta) + B*sin(k*theta)).
        const double r=std::sqrt(a[1]),cosine=-a[0]/(2*r),sine=std::sqrt(1-cosine*cosine);
        const double h0=b[0],h1=b[1]-a[0]*h0,h2=b[2]-a[0]*h1-a[1]*h0;
        const double coefficient=(h2/r-h1*cosine)/sine;
        const double gain=std::abs(h0)+(std::abs(h1)+std::abs(coefficient))/(1-r);
        const double outputBound=historyBound*gain;
        delay2Bound=historyBound*std::abs(b[2])+std::abs(a[1])*outputBound;
        delay1Bound=historyBound*std::abs(b[1])+std::abs(a[0])*outputBound+delay2Bound;
        delay2Bound*=1+1e-8;delay1Bound*=1+1e-8;
        if(filteredHistory) historyBound=outputBound*(1+1e-8);
    }
    for(size_t age=0;age<count;++age) {
        const double value=std::bit_cast<double>(get(bytes,at));
        if(!std::isfinite(value) || (age>=state.nativeIndex && value!=0) ||
            (filteredHistory?std::abs(value)>historyBound:value<static_cast<double>(std::numeric_limits<int32_t>::min()) || value>static_cast<double>(std::numeric_limits<int32_t>::max()) || std::trunc(value)!=value)) return std::nullopt;
        state.history[(state.next+32-age)%33]=value;
    }
    if(method==FmInterpolation::ZohLowPass) {
        state.delay1=std::bit_cast<double>(get(bytes,at));state.delay2=std::bit_cast<double>(get(bytes,at));
        if(!std::isfinite(state.delay1) || !std::isfinite(state.delay2) || std::abs(state.delay1)>delay1Bound || std::abs(state.delay2)>delay2Bound) return std::nullopt;
        if((!state.nativeIndex || !(filteredHistory?state.nativeIndex:state.outputIndex)) && (state.delay1!=0 || state.delay2!=0)) return std::nullopt;
    }
    return state;
}
} // namespace smsgg
