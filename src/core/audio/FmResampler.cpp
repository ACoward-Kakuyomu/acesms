#include "core/audio/FmResampler.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace smsgg {
FmResampler::FmResampler(uint32_t masterClock,uint32_t outputRate,FmInterpolation method)
    : clock_(masterClock),rate_(outputRate),method_(method) {
    const double nativeRate=static_cast<double>(clock_)/72;
    const double hz=.45*std::min(nativeRate,static_cast<double>(rate_));
    cutoff_=hz/nativeRate;
    if(method_==FmInterpolation::ZohLowPass) {
        const double processRate=std::max(nativeRate,static_cast<double>(rate_));
        const double k=std::tan(std::numbers::pi*hz/processRate);
        const double norm=1/(1+std::numbers::sqrt2*k+k*k);
        b_={k*k*norm,2*k*k*norm,k*k*norm};
        a_={2*(k*k-1)*norm,(1-std::numbers::sqrt2*k+k*k)*norm};
    }
}
double FmResampler::filter(double input) {
    const double output=b_[0]*input+state_.delay1;
    state_.delay1=b_[1]*input-a_[0]*output+state_.delay2;
    state_.delay2=b_[2]*input-a_[1]*output;
    return output;
}
void FmResampler::push(int32_t nativeSample) {
    double x=nativeSample;
    if(method_==FmInterpolation::ZohLowPass && uint64_t(rate_)*72<clock_) x=filter(x);
    state_.history[state_.next]=x;
    state_.next=static_cast<uint8_t>((state_.next+1)%state_.history.size());
    ++state_.nativeIndex;
}
double FmResampler::value(uint64_t index) const {
    if(index==0) return 0;
    assert(index<=state_.nativeIndex && state_.nativeIndex-index<state_.history.size());
    const auto age=static_cast<unsigned>(state_.nativeIndex-index);
    return state_.history[(state_.next+32-age)%state_.history.size()];
}
double FmResampler::sample() {
    const uint64_t denominator=uint64_t(rate_)*72;
    ++state_.outputIndex;
    state_.position+=clock_/denominator;
    state_.phase+=clock_%denominator;
    if(state_.phase>=denominator) {state_.phase-=denominator;++state_.position;}
    const auto p=state_.position;
    const double f=static_cast<double>(state_.phase)/static_cast<double>(denominator);
    if(method_==FmInterpolation::Linear)
        return (p?value(p-1):0)*(1-f)+value(p)*f;
    if(method_==FmInterpolation::ZohLowPass) {
        const double held=value(p);
        return uint64_t(rate_)*72<clock_?held:filter(held);
    }
    double sum=0,weight=0;
    for(unsigned j=0;j<32;++j) {
        const double d=f-16+j;
        if(std::abs(d)>=16) continue;
        const double x=2*cutoff_*d;
        const double sinc=x==0?1:std::sin(std::numbers::pi*x)/(std::numbers::pi*x);
        const double window=.42+.5*std::cos(std::numbers::pi*d/16)+.08*std::cos(2*std::numbers::pi*d/16);
        const double w=2*cutoff_*sinc*window;
        weight+=w;
        if(p>j) sum+=w*value(p-j);
    }
    return sum/weight;
}
} // namespace smsgg
