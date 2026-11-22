#include "win32/AudioFormat.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>

namespace smsgg::win32 {
std::variant<AudioFormat,std::string> AudioFormat::parse(std::span<const std::byte> bytes) {
    if(bytes.size()<sizeof(WAVEFORMATEX)) return "Truncated WAVEFORMATEX";
    WAVEFORMATEX w{};std::memcpy(&w,bytes.data(),sizeof(w));
    if(w.cbSize>bytes.size()-sizeof(w)) return "Truncated audio format extension";
    if(!w.nSamplesPerSec || w.nSamplesPerSec>3546893) return "Invalid audio sample rate";
    if(!w.nChannels || w.nChannels>32) return "Unsupported audio channel count";
    bool floating=w.wFormatTag==WAVE_FORMAT_IEEE_FLOAT;
    uint16_t valid=w.wBitsPerSample;
    uint32_t mask=0;
    if(w.wFormatTag==WAVE_FORMAT_EXTENSIBLE) {
        if(w.cbSize<22) return "Missing extensible audio format";
        WAVEFORMATEXTENSIBLE ext{};std::memcpy(&ext,bytes.data(),sizeof(ext));
        if(ext.SubFormat==KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) floating=true;
        else if(ext.SubFormat!=KSDATAFORMAT_SUBTYPE_PCM) return "Unsupported audio subtype";
        valid=ext.Samples.wValidBitsPerSample;mask=ext.dwChannelMask;
    } else if(w.wFormatTag!=WAVE_FORMAT_PCM && !floating) return "Unsupported audio format tag";
    if((floating && (w.wBitsPerSample!=32 || valid!=32))
        || (!floating && w.wBitsPerSample!=16 && w.wBitsPerSample!=24 && w.wBitsPerSample!=32)
        || !valid || valid>w.wBitsPerSample) return "Unsupported audio sample container/valid bits";
    const auto block=w.nChannels*(w.wBitsPerSample/8);
    if(w.nBlockAlign!=block || uint64_t(w.nAvgBytesPerSec)!=uint64_t(block)*w.nSamplesPerSec)
        return "Invalid audio block alignment/byte rate";
    if(mask) {
        if(std::popcount(mask)!=w.nChannels || (w.nChannels>1 && (mask&3)!=3)) return "Unsupported speaker layout";
    } else if(w.nChannels>2) return "Missing multichannel speaker mask";
    AudioFormat result;
    result.rate_=w.nSamplesPerSec;result.channels_=w.nChannels;result.block_=w.nBlockAlign;
    result.bits_=w.wBitsPerSample;result.valid_=valid;result.floating_=floating;
    return result;
}

void AudioFormat::write(std::span<const AudioFrame> frames,std::span<std::byte> output,bool mute) const {
    assert(output.size()%block_==0 && frames.size()<=output.size()/block_);
    std::fill(output.begin(),output.end(),std::byte{});
    if(mute) return;
    const auto sampleBytes=bits_/8;
    for(size_t i=0;i<frames.size();++i) for(unsigned ch=0;ch<std::min<unsigned>(channels_,2);++ch) {
        const float mixed=channels_==1?std::midpoint(frames[i].left,frames[i].right):ch==0?frames[i].left:frames[i].right;
        const float sample=std::isfinite(mixed)?std::clamp(mixed,-1.0f,1.0f):0.0f;
        auto* destination=output.data()+i*block_+ch*sampleBytes;
        if(floating_) std::memcpy(destination,&sample,sizeof(sample));
        else {
            const auto scale=int64_t{1}<<(valid_-1);
            const auto value=std::clamp(std::llround(double(sample)*double(scale)),-scale,scale-1);
            const auto packed=static_cast<uint32_t>(value)<<(bits_-valid_);
            for(unsigned b=0;b<sampleBytes;++b) destination[b]=static_cast<std::byte>(packed>>(b*8));
        }
    }
}
} // namespace smsgg::win32
