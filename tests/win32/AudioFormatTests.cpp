#include "win32/AudioFormat.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {
using smsgg::win32::AudioFormat;
WAVEFORMATEXTENSIBLE wave(unsigned bits=32,unsigned valid=32,unsigned channels=2,bool floating=true) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format={WAVE_FORMAT_EXTENSIBLE,static_cast<WORD>(channels),44100,
        44100*channels*bits/8,static_cast<WORD>(channels*bits/8),static_cast<WORD>(bits),22};
    w.Samples.wValidBitsPerSample=static_cast<WORD>(valid);
    w.dwChannelMask=channels==1?SPEAKER_FRONT_CENTER:channels==2?3:0x3f;
    w.SubFormat=floating?KSDATAFORMAT_SUBTYPE_IEEE_FLOAT:KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}
bool rejected(const WAVEFORMATEXTENSIBLE& w) {return std::holds_alternative<std::string>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));}
bool formats() {
    for(const unsigned bits:{16u,24u,32u}) for(const unsigned channels:{1u,2u,6u}) {
        auto w=wave(bits,bits,channels,false);
        const auto result=AudioFormat::parse(std::as_bytes(std::span(&w,1)));
        if(!std::holds_alternative<AudioFormat>(result)) return false;
        const auto f=std::get<AudioFormat>(result);
        if(f.sampleRate()!=44100 || f.channels()!=channels || f.blockAlign()!=channels*bits/8) return false;
    }
    auto w=wave();w.Format.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;w.Format.cbSize=0;
    if(!std::holds_alternative<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w.Format,1))))) return false;
    w=wave(16,16,2,false);w.Format.wFormatTag=WAVE_FORMAT_PCM;w.Format.cbSize=0;
    return std::holds_alternative<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w.Format,1))));
}
bool validation() {
    auto w=wave();
    const auto raw=std::as_bytes(std::span(&w,1));
    if(!std::holds_alternative<std::string>(AudioFormat::parse(raw.first(10)))
        || !std::holds_alternative<std::string>(AudioFormat::parse(raw.first(39)))) return false;
    w.Format.nSamplesPerSec=0;if(!rejected(w)) return false;
    w=wave();w.Format.nSamplesPerSec=4000000;if(!rejected(w)) return false;
    w=wave();w.Format.nBlockAlign=7;if(!rejected(w)) return false;
    w=wave();w.Format.nAvgBytesPerSec=1;if(!rejected(w)) return false;
    w=wave();w.Format.cbSize=1;if(!rejected(w)) return false;
    w=wave(8,8,2,false);if(!rejected(w)) return false;
    w=wave(64,64,2,true);if(!rejected(w)) return false;
    w=wave(32,33,2,false);if(!rejected(w)) return false;
    w=wave(32,0,2,false);if(!rejected(w)) return false;
    w=wave();w.SubFormat=GUID_NULL;if(!rejected(w)) return false;
    w=wave();w.dwChannelMask=0x30;if(!rejected(w)) return false;
    w=wave();w.dwChannelMask=7;if(!rejected(w)) return false;
    w=wave(32,32,6,true);w.dwChannelMask=0;if(!rejected(w)) return false;
    w=wave();w.Format.nChannels=0;return rejected(w);
}
bool encoding() {
    const std::array<smsgg::AudioFrame,1> extrema{{{-2,2}}};
    const std::vector<std::vector<unsigned char>> expected{{0,0x80,0xff,0x7f},
        {0,0,0x80,0xff,0xff,0x7f},{0,0,0,0x80,0xff,0xff,0xff,0x7f}};
    unsigned i=0;
    for(const unsigned bits:{16u,24u,32u}) {
        auto w=wave(bits,bits,2,false);
        auto f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
        std::vector<std::byte> out(f.blockAlign()*2,std::byte{0x55});
        f.write(extrema,out,false);
        if(std::memcmp(out.data(),expected[i].data(),expected[i].size())!=0) return false;
        for(size_t j=expected[i++].size();j<out.size();++j) if(out[j]!=std::byte{}) return false;
        f.write(extrema,out,true);for(auto b:out) if(b!=std::byte{}) return false;
    }
    auto w=wave(32,24,2,false);
    auto f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
    std::array<std::byte,8> out{};f.write(extrema,out,false);
    const std::array<unsigned char,8> leftAligned{0,0,0,0x80,0,0xff,0xff,0x7f};
    if(std::memcmp(out.data(),leftAligned.data(),8)!=0) return false;
    w=wave(16,16,2,false);f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
    const std::array<smsgg::AudioFrame,1> ties{{{.5f/32768,-.5f/32768}}};f.write(ties,std::span(out).first(4),false);
    const std::array<unsigned char,4> rounded{1,0,0xff,0xff};if(std::memcmp(out.data(),rounded.data(),4)!=0) return false;
    const std::array<smsgg::AudioFrame,1> stereo{{{.25f,.75f}}};
    for(const unsigned channels:{1u,2u,6u}) {
        w=wave(32,32,channels,true);f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
        std::vector<std::byte> bytes(f.blockAlign());f.write(stereo,bytes,false);
        for(unsigned ch=0;ch<channels;++ch) {
            float actual{};std::memcpy(&actual,bytes.data()+ch*4,4);
            const float want=channels==1?.5f:ch==0?.25f:ch==1?.75f:0;
            if(actual!=want) return false;
        }
    }
    return true;
}
bool sampleRange() {
    const auto infinity=std::numeric_limits<float>::infinity();
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    const std::array<smsgg::AudioFrame,3> input{{{-2,2},{nan,infinity},{-infinity,.25f}}};
    auto w=wave();
    auto f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
    std::array<float,6> floating{};
    f.write(input,std::as_writable_bytes(std::span(floating)),false);
    if(floating!=std::array<float,6>{-1,1,0,0,0,.25f}) return false;
    w=wave(16,16,2,false);
    f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
    std::array<int16_t,6> pcm{};
    f.write(input,std::as_writable_bytes(std::span(pcm)),false);
    if(pcm!=std::array<int16_t,6>{-32768,32767,0,0,0,8192}) return false;
    w=wave(32,32,1);
    f=std::get<AudioFormat>(AudioFormat::parse(std::as_bytes(std::span(&w,1))));
    const auto largest=std::numeric_limits<float>::max();
    const std::array<smsgg::AudioFrame,3> mono{{{largest,largest},{2,-1},{.5f,nan}}};
    std::array<float,3> downmixed{};
    f.write(mono,std::as_writable_bytes(std::span(downmixed)),false);
    return downmixed==std::array<float,3>{1,.5f,0};
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="formats"?formats():name=="validation"?validation():name=="encoding"&&encoding()&&sampleRange();
    if(!pass) std::cerr<<"Audio format test failed: "<<name<<'\n';
    return pass?0:1;
}
