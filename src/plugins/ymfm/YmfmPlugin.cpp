// MIT License. Copyright (c) 2026 ACoward. ymfm remains BSD-3-Clause.
#include "acesms_fm.h"
#include <ymfm_opl.h>

#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <vector>

namespace {
constexpr uint32_t payloadBytes=319,stateBytes=327;
struct Chip {
    ymfm::ymfm_interface host;
    ymfm::ym2413 engine{host};
    Chip() {engine.reset();}
};
struct Instance {std::unique_ptr<Chip> sound=std::make_unique<Chip>();uint64_t time{};};
Instance* instance(acesms_fm_instance* h) {return reinterpret_cast<Instance*>(h);}
uint64_t read(std::span<const uint8_t> bytes,size_t offset,unsigned width) {
    uint64_t value=0;for(unsigned i=0;i<width;++i) value|=uint64_t(bytes[offset+i])<<(8*i);return value;
}
bool valid(std::span<const uint8_t> bytes,uint64_t expectedTime) {
    if(bytes.size()!=stateBytes || read(bytes,0,8)!=expectedTime) return false;
    const auto native=expectedTime/72;
    if(read(bytes,9,4)!=static_cast<uint32_t>(native*4) || bytes[13] || bytes[14]!=4 || bytes[15] || bytes[16]
        || bytes[17] || bytes[18] || bytes[19]!=static_cast<uint8_t>(native)
        || read(bytes,20,2)!=native%13440 || read(bytes,22,2)!=(native&0xffff) || !(read(bytes,25,4)&0x7fffff)) return false;
    const auto previous=native?(native-1)%13440:0;
    const auto am=(previous<6720?previous:13503-previous)>>7;
    if(bytes[24]!=am) return false;
    for(unsigned op=0;op<18;++op) {
        const auto offset=147+op*10;
        if(read(bytes,offset+4,2)>1023 || bytes[offset+6]>4 || bytes[offset+7] || bytes[offset+8]>1 || bytes[offset+9]>3) return false;
    }
    return true;
}
acesms_fm_result ACESMS_FM_CALL create(const acesms_fm_config* config,acesms_fm_instance** out) noexcept {
    if(!out) return ACESMS_FM_INVALID_ARGUMENT;
    *out=nullptr;
    if(!config || config->struct_size!=sizeof(*config)) return ACESMS_FM_INVALID_ARGUMENT;
    if(config->master_clock_hz!=3579545 || config->native_divider!=72) return ACESMS_FM_UNSUPPORTED_CONFIG;
    try {*out=reinterpret_cast<acesms_fm_instance*>(new Instance());}
    catch(const std::bad_alloc&) {return ACESMS_FM_OUT_OF_MEMORY;}
    return ACESMS_FM_OK;
}
void ACESMS_FM_CALL destroy(acesms_fm_instance* h) noexcept {delete instance(h);}
acesms_fm_result ACESMS_FM_CALL reset(acesms_fm_instance* h) noexcept {
    if(!h) return ACESMS_FM_INVALID_ARGUMENT;
    try {
        auto fresh=std::make_unique<Chip>();instance(h)->sound=std::move(fresh);instance(h)->time=0;
    } catch(const std::bad_alloc&) {return ACESMS_FM_OUT_OF_MEMORY;}
    return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL advanceTo(acesms_fm_instance* h,uint64_t target,int32_t* out,uint32_t capacity,uint32_t* produced) noexcept {
    if(!produced) return ACESMS_FM_INVALID_ARGUMENT;
    *produced=0;
    if(!h || target<instance(h)->time) return ACESMS_FM_INVALID_ARGUMENT;
    auto& self=*instance(h);const auto count=target/72-self.time/72;
    if(count>capacity) return ACESMS_FM_BUFFER_TOO_SMALL;
    if(count && !out) return ACESMS_FM_INVALID_ARGUMENT;
    for(uint32_t i=0;i<count;++i) {
        ymfm::ym2413::output_data sample;self.sound->engine.generate(&sample);
        out[i]=sample.data[0]+sample.data[1];
    }
    self.time=target;*produced=static_cast<uint32_t>(count);return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL writeIo(acesms_fm_instance* h,uint32_t port,uint8_t value) noexcept {
    if(!h || port>1) return ACESMS_FM_INVALID_ARGUMENT;
    instance(h)->sound->engine.write(port,value);return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL stateSize(acesms_fm_instance* h,uint32_t* bytes) noexcept {
    if(!bytes) return ACESMS_FM_INVALID_ARGUMENT;
    *bytes=0;if(!h) return ACESMS_FM_INVALID_ARGUMENT;
    *bytes=stateBytes;return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL saveState(acesms_fm_instance* h,uint8_t* bytes,uint32_t capacity,uint32_t* written) noexcept {
    if(!written) return ACESMS_FM_INVALID_ARGUMENT;
    *written=0;if(!h) return ACESMS_FM_INVALID_ARGUMENT;
    if(capacity<stateBytes) return ACESMS_FM_BUFFER_TOO_SMALL;
    if(!bytes) return ACESMS_FM_INVALID_ARGUMENT;
    try {
        std::vector<uint8_t> payload;payload.reserve(payloadBytes);
        ymfm::ymfm_saved_state state(payload,true);instance(h)->sound->engine.save_restore(state);
        if(payload.size()!=payloadBytes) return ACESMS_FM_INVALID_STATE;
        for(unsigned i=0;i<8;++i) bytes[i]=static_cast<uint8_t>(instance(h)->time>>(8*i));
        std::memcpy(bytes+8,payload.data(),payloadBytes);*written=stateBytes;
    } catch(const std::bad_alloc&) {return ACESMS_FM_OUT_OF_MEMORY;}
    return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL loadState(acesms_fm_instance* h,const uint8_t* bytes,uint32_t length,const acesms_fm_load_context* context) noexcept {
    if(!h || !context || context->struct_size!=sizeof(*context) || (!bytes && length)) return ACESMS_FM_INVALID_ARGUMENT;
    if(context->master_clock_hz!=3579545 || context->native_divider!=72) return ACESMS_FM_UNSUPPORTED_CONFIG;
    const std::span<const uint8_t> input(bytes,length);
    if(!valid(input,context->master_ticks)) return ACESMS_FM_INVALID_STATE;
    try {
        auto candidate=std::make_unique<Chip>();std::vector<uint8_t> payload(input.begin()+8,input.end());
        ymfm::ymfm_saved_state state(payload,false);candidate->engine.save_restore(state);
        instance(h)->sound=std::move(candidate);instance(h)->time=context->master_ticks;
    } catch(const std::bad_alloc&) {return ACESMS_FM_OUT_OF_MEMORY;}
    return ACESMS_FM_OK;
}
const acesms_fm_api api{
    ACESMS_FM_ABI_VERSION,sizeof(acesms_fm_api),
    {{0x61,0x63,0x65,0x73,0x6d,0x73,0x2d,0x79,0x6d,0x66,0x6d,0,0,0,0,1},
     "ymfm OPLL","81aec25ccbb98f4873a255f7551ac4dadac59b4a",1,
     {0x79,0x6d,0x66,0x6d,0x2d,0x6f,0x70,0x6c,0x6c,0x2d,0x76,0x31,0,0,0,1},3579545,72},
    create,destroy,reset,advanceTo,writeIo,stateSize,saveState,loadState
};
} // namespace
extern "C" ACESMS_FM_EXPORT const acesms_fm_api* ACESMS_FM_CALL acesms_fm_get_api(uint32_t requestedAbi) {
    return requestedAbi==ACESMS_FM_ABI_VERSION?&api:nullptr;
}
