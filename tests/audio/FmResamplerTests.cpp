#include "core/audio/FmResampler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <string_view>
#include <vector>

namespace {
using namespace smsgg;
constexpr std::array methods{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear};
bool near(double a,double b,double tolerance=1e-8) {return std::abs(a-b)<tolerance;}
template<class Input>
std::vector<double> render(uint32_t clock,uint32_t rate,FmInterpolation method,size_t count,Input input) {
    FmResampler converter(clock,rate,method);
    uint64_t native=0;
    std::vector<double> out;out.reserve(count);
    for(uint64_t n=1;n<=count;++n) {
        const auto due=(n*clock+rate-1)/rate;
        while(native<due/72) converter.push(input(++native));
        out.push_back(converter.sample());
    }
    return out;
}
bool vectors() {
    for(auto method:methods) {
        const auto impulse=render(72000,1000,method,80,[](uint64_t k){return k==1?32768:0;});
        if(method==FmInterpolation::Linear) {
            for(size_t i=0;i<impulse.size();++i) if(impulse[i]!=(i==1?32768:0)) return false;
        } else if(method==FmInterpolation::Sinc) {
            const auto peak=std::max_element(impulse.begin(),impulse.end())-impulse.begin();
            double sum=0;for(auto y:impulse) sum+=y;
            if(peak!=16 || !near(sum,32768,1e-7)) return false;
            for(unsigned d=1;d<16;++d) if(!near(impulse[16-d],impulse[16+d])) return false;
        } else {
            // Independently evaluated bilinear Butterworth impulse at cutoff=.45*rate.
            constexpr std::array expected{.8005924034645703,.35144559377220475,-.26148169039356084,.18277747308591852,-.11761725502849638};
            for(size_t i=0;i<expected.size();++i) if(!near(impulse[i]/32768,expected[i])) return false;
        }
        for(uint32_t rate:{44100u,48000u,96000u}) {
            const auto dc=render(3579545,rate,method,1000,[](auto){return 12345;});
            for(size_t i=500;i<dc.size();++i) if(!near(dc[i],12345,1e-7)) return false;
        }
    }
    const auto ramp=render(72000,2000,FmInterpolation::Linear,100,[](uint64_t k){return static_cast<int32_t>(100*k);});
    for(size_t i=0;i<ramp.size();++i) if(!near(ramp[i],i==0?0:50.0*(i-1))) return false;
    // At T=8280 the newest native value is 115, but output 102 needs oldest tap 83.
    const auto boundary=render(3579545,44100,FmInterpolation::Sinc,102,[](uint64_t k){return k==83?32768:0;});
    if(!near(boundary.back(),.00017907778676812814,1e-10)) return false;
    return true;
}
bool state() {
    for(auto method:methods) for(uint32_t rate:{44100u,48000u,96000u}) {
        FmResampler original(3579545,rate,method),restored(3579545,rate,method);
        uint64_t native=0;
        const auto input=[](uint64_t k){return static_cast<int32_t>((k*7919)%65536)-32768;};
        for(uint64_t n=1;n<=103;++n) {
            const auto due=(n*3579545+rate-1)/rate;
            while(native<due/72) original.push(input(++native));
            original.sample();
        }
        const auto saved=original.captureState();
        if(saved.outputIndex!=103 || saved.nativeIndex!=native || saved.position!=103ull*3579545/(72*rate)
            || saved.phase!=103ull*3579545%(72*rate)) return false;
        // Restore over an already-used converter, including its nonzero LPF delays.
        for(uint64_t n=1,k=0;n<=50;++n) {
            const auto due=(n*3579545+rate-1)/rate;
            while(k<due/72) {restored.push(1234);++k;}
            restored.sample();
        }
        restored.restoreState(saved);
        for(uint64_t n=104;n<=503;++n) {
            const auto due=(n*3579545+rate-1)/rate;
            while(native<due/72) {const auto x=input(++native);original.push(x);restored.push(x);}
            if(original.sample()!=restored.sample()) return false;
        }
        restored.restoreState(saved);
        if(restored.captureState().history!=saved.history || saved.outputIndex!=103) return false;
        restored.reset();FmResampler fresh(3579545,rate,method);
        for(uint64_t n=1,k=0;n<=100;++n) {
            const auto due=(n*3579545+rate-1)/rate;
            while(k<due/72) {const auto x=input(++k);restored.push(x);fresh.push(x);}
            if(restored.sample()!=fresh.sample()) return false;
        }
    }
    return true;
}
bool quality() {
    for(uint32_t rate:{44100u,48000u,96000u}) for(auto method:methods) {
        const auto start=std::chrono::steady_clock::now();
        const double nativeRate=3579545.0/72;
        for(double frequency:std::array{100.0,1000.0,8000.0,18000.0,.495*std::min(nativeRate,static_cast<double>(rate)),24000.0}) {
            const auto wave=render(3579545,rate,method,rate/4,[&](uint64_t k) {
                return static_cast<int32_t>(std::round(32768*std::sin(2*std::numbers::pi*frequency*72*static_cast<double>(k)/3579545)));
            });
            double energy=0;
            for(size_t i=rate/20;i<wave.size();++i) {
                if(!std::isfinite(wave[i])) return false;
                energy+=wave[i]*wave[i];
            }
            const double gain=std::sqrt(2*energy/static_cast<double>(wave.size()-rate/20))/32768;
            std::cout<<"rate="<<rate<<" method="<<static_cast<unsigned>(method)<<" Hz="<<frequency<<" gain="<<gain<<'\n';
            if(frequency==1000 && (gain<.97 || gain>1.02)) return false;
            if(rate==44100 && frequency==24000) {
                if(method==FmInterpolation::Sinc && gain>.05) return false;
                if(method==FmInterpolation::ZohLowPass && gain>.08) return false;
                if(method==FmInterpolation::Linear && gain<.25) return false;
            }
        }
        // Linear chirp from 100Hz to 98% of native Nyquist, over one second.
        const auto sweep=render(3579545,rate,method,rate,[&](uint64_t k) {
            const double t=72*static_cast<double>(k)/3579545;
            return static_cast<int32_t>(std::round(32768*std::sin(2*std::numbers::pi*(100*t+.5*(.49*nativeRate-100)*t*t))));
        });
        double sweepEnergy=0,peak=0;
        for(auto y:sweep) {if(!std::isfinite(y)) return false;sweepEnergy+=y*y;peak=std::max(peak,std::abs(y));}
        const double rms=std::sqrt(sweepEnergy/sweep.size())/32768;
        if(rms<.2 || rms>1 || peak>65536) return false;
        std::cout<<"rate="<<rate<<" method="<<static_cast<unsigned>(method)<<" chirp_rms="<<rms<<" peak="<<peak<<'\n';
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"rate="<<rate<<" method="<<static_cast<unsigned>(method)<<" response_and_chirp_ms="<<ms<<" generated_seconds=2.5\n";
    }
    return true;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="vectors"?vectors():name=="state"?state():name=="quality"&&quality();
    if(!pass) std::cerr<<"FM resampler test failed: "<<name<<'\n';
    return pass?0:1;
}
