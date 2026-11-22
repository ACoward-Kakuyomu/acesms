#include "core/audio/FmResampler.h"

#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {
using namespace smsgg;
constexpr uint32_t clockHz=3579545;
constexpr std::array methods{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear};
uint64_t count(uint64_t time,uint32_t rate) {return time/clockHz*rate+(time%clockHz)*rate/clockHz;}
std::vector<double> advance(FmResampler& converter,uint64_t& time,uint64_t target,uint32_t rate,bool fullScale=false) {
    std::vector<double> output;
    while(time<target) {
        const auto old=count(time,rate);++time;
        if(time%72==0) converter.push(fullScale?(time/72%5?std::numeric_limits<int32_t>::max():std::numeric_limits<int32_t>::min()):static_cast<int32_t>(((time/72)%65536*7919)%65536)-32768);
        if(count(time,rate)!=old) output.push_back(converter.sample());
    }
    return output;
}
std::vector<uint8_t> encode(const FmResamplerState& state,FmInterpolation method) {
    std::vector<uint8_t> bytes(FmResampler::stateBytes(method));FmResampler::encodeState(state,method,bytes);return bytes;
}
void putDouble(std::vector<uint8_t>& bytes,size_t at,double value) {
    auto bits=std::bit_cast<uint64_t>(value);
    for(unsigned n=0;n<8;++n) {bytes[at+n]=static_cast<uint8_t>(bits);bits>>=8;}
}
bool bytes() {
    if(FmResampler::stateBytes(FmInterpolation::Sinc)!=296 || FmResampler::stateBytes(FmInterpolation::ZohLowPass)!=64 || FmResampler::stateBytes(FmInterpolation::Linear)!=56) return false;
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::Linear}) {
        FmResampler converter(clockHz,48000,method);converter.push(2);
        const auto actual=encode(converter.captureState(),method);
        std::vector<uint8_t> expected(actual.size());expected[0]=1;expected[39]=0x40;
        if(actual!=expected) return false;
        const auto decoded=FmResampler::decodeState(actual,clockHz,48000,method,72);
        if(!decoded || decoded->nativeIndex!=1 || decoded->history[0]!=2 || decoded->next!=1) return false;
    }
    return true;
}
bool reject() {
    for(auto method:methods) for(uint32_t rate:{44100u,48000u,96000u}) {
        FmResampler converter(clockHz,rate,method);uint64_t time=0;advance(converter,time,140003,rate);
        const auto original=encode(converter.captureState(),method);
        for(size_t n=0;n<original.size();++n)
            if(FmResampler::decodeState(std::span(original).first(n),clockHz,rate,method,time)) return false;
        auto bad=original;bad.push_back(0);
        if(FmResampler::decodeState(bad,clockHz,rate,method,time)) return false;
        for(unsigned field=0;field<4;++field) {
            bad=original;bad[field*8]^=1;
            if(FmResampler::decodeState(bad,clockHz,rate,method,time)) return false;
        }
        for(double value:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),1e100}) {
            bad=original;putDouble(bad,32,value);
            if(FmResampler::decodeState(bad,clockHz,rate,method,time)) return false;
            if(method==FmInterpolation::ZohLowPass) {
                for(size_t offset:{48u,56u}) {
                    bad=original;putDouble(bad,offset,value);
                    if(FmResampler::decodeState(bad,clockHz,rate,method,time)) return false;
                }
            }
        }
        if(method!=FmInterpolation::ZohLowPass || uint64_t(rate)*72>=clockHz) {
            bad=original;putDouble(bad,32,.5);
            if(FmResampler::decodeState(bad,clockHz,rate,method,time)) return false;
        }
        FmResampler initial(clockHz,rate,method);bad=encode(initial.captureState(),method);putDouble(bad,32,1);
        if(FmResampler::decodeState(bad,clockHz,rate,method,0)) return false;
        if(method==FmInterpolation::ZohLowPass) {
            bad=encode(initial.captureState(),method);putDouble(bad,48,1);
            if(FmResampler::decodeState(bad,clockHz,rate,method,0)) return false;
        }
    }
    return true;
}
bool replay() {
    for(auto method:methods) for(uint32_t rate:{44100u,48000u,96000u})
    for(uint64_t cut:{0ull,1ull,71ull,72ull,8280ull,140003ull}) {
        FmResampler source(clockHz,rate,method),restored(clockHz,rate,method);uint64_t time=0;advance(source,time,cut,rate);
        const auto original=encode(source.captureState(),method);
        const auto state=FmResampler::decodeState(original,clockHz,rate,method,cut);
        if(!state) return false;
        restored.restoreState(*state);auto otherTime=time;
        if(encode(*state,method)!=original || encode(source.captureState(),method)!=original || advance(source,time,cut+100000,rate)!=advance(restored,otherTime,cut+100000,rate)) return false;
    }
    for(auto method:methods) for(uint32_t rate:{1u,8000u,192000u,clockHz}) {
        FmResampler a(clockHz,rate,method),b(clockHz,rate,method);uint64_t time=0;advance(a,time,10000,rate,true);
        const auto state=FmResampler::decodeState(encode(a.captureState(),method),clockHz,rate,method,time);
        if(!state) return false;
        b.restoreState(*state);auto otherTime=time;
        if(advance(a,time,20000,rate,true)!=advance(b,otherTime,20000,rate,true)) return false;
    }
    return true;
}
bool epoch() {
    // Expected quotients/remainders computed independently with arbitrary-precision integers.
    constexpr uint64_t time=18446744073708551615ull,native=256204778801507661ull;
    constexpr std::array<std::array<uint64_t,4>,3> expected{{
        {44100,227263915846999304ull,256204778801507660ull,1604680},
        {48000,247362085275645501ull,256204778801507660ull,1917045},
        {96000,494724170551291003ull,256204778801507661ull,501635}}};
    for(auto method:methods) for(auto row:expected) {
        const auto rate=static_cast<uint32_t>(row[0]);
        FmResamplerState state{.nativeIndex=native,.outputIndex=row[1],.position=row[2],.phase=row[3],.next=static_cast<uint8_t>(native%33)};
        const auto decoded=FmResampler::decodeState(encode(state,method),clockHz,rate,method,time);
        if(!decoded || decoded->position!=state.position || decoded->phase!=state.phase || decoded->outputIndex!=state.outputIndex) return false;
        FmResampler a(clockHz,rate,method),b(clockHz,rate,method);a.restoreState(state);b.restoreState(*decoded);
        auto t1=time,t2=time;
        if(advance(a,t1,time+100000,rate)!=advance(b,t2,time+100000,rate)) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    const std::string_view operation=argc==2?argv[1]:"";
    const bool passed=operation=="bytes"?bytes():operation=="reject"?reject():operation=="replay"?replay():operation=="epoch"&&epoch();
    if(!passed) std::cerr<<"FM resampler codec failed: "<<operation<<'\n';return passed?0:1;
}
