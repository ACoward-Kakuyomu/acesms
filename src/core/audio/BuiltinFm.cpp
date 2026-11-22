#include "core/audio/BuiltinFm.h"
#include "core/audio/Emu2413Native.h"

#include <new>

namespace smsgg {
namespace {
Emu2413Native* chip(acesms_fm_instance* instance) {return reinterpret_cast<Emu2413Native*>(instance);}
acesms_fm_result ACESMS_FM_CALL create(const acesms_fm_config* config,acesms_fm_instance** out) noexcept {
    if(!out) return ACESMS_FM_INVALID_ARGUMENT;
    *out=nullptr;
    if(!config || config->struct_size!=sizeof(*config)) return ACESMS_FM_INVALID_ARGUMENT;
    if(config->master_clock_hz!=3579545 || config->native_divider!=72) return ACESMS_FM_UNSUPPORTED_CONFIG;
    try {*out=reinterpret_cast<acesms_fm_instance*>(new Emu2413Native());}
    catch(const std::bad_alloc&) {return ACESMS_FM_OUT_OF_MEMORY;}
    return ACESMS_FM_OK;
}
void ACESMS_FM_CALL destroy(acesms_fm_instance* instance) noexcept {delete chip(instance);}
acesms_fm_result ACESMS_FM_CALL reset(acesms_fm_instance* instance) noexcept {
    if(!instance) return ACESMS_FM_INVALID_ARGUMENT;
    chip(instance)->reset();return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL advanceTo(acesms_fm_instance* instance,uint64_t target,int32_t* out,uint32_t capacity,uint32_t* produced) noexcept {
    if(!produced) return ACESMS_FM_INVALID_ARGUMENT;
    *produced=0;
    if(!instance || target<chip(instance)->masterTicks()) return ACESMS_FM_INVALID_ARGUMENT;
    const auto count=target/72-chip(instance)->masterTicks()/72;
    if(count>capacity) return ACESMS_FM_BUFFER_TOO_SMALL;
    if(count && !out) return ACESMS_FM_INVALID_ARGUMENT;
    *produced=static_cast<uint32_t>(chip(instance)->advanceTo(target,{out,static_cast<size_t>(count)}));
    return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL writeIo(acesms_fm_instance* instance,uint32_t port,uint8_t value) noexcept {
    if(!instance || port>1) return ACESMS_FM_INVALID_ARGUMENT;
    chip(instance)->writeIo(static_cast<uint8_t>(port),value);return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL stateSize(acesms_fm_instance* instance,uint32_t* bytes) noexcept {
    if(!bytes) return ACESMS_FM_INVALID_ARGUMENT;
    *bytes=0;
    if(!instance) return ACESMS_FM_INVALID_ARGUMENT;
    *bytes=Emu2413Native::stateBytes;return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL saveState(acesms_fm_instance* instance,uint8_t* bytes,uint32_t capacity,uint32_t* written) noexcept {
    if(!written) return ACESMS_FM_INVALID_ARGUMENT;
    *written=0;
    if(!instance) return ACESMS_FM_INVALID_ARGUMENT;
    if(capacity<Emu2413Native::stateBytes) return ACESMS_FM_BUFFER_TOO_SMALL;
    if(!bytes) return ACESMS_FM_INVALID_ARGUMENT;
    chip(instance)->saveState({bytes,Emu2413Native::stateBytes});
    *written=Emu2413Native::stateBytes;return ACESMS_FM_OK;
}
acesms_fm_result ACESMS_FM_CALL loadState(acesms_fm_instance* instance,const uint8_t* bytes,uint32_t length,const acesms_fm_load_context* context) noexcept {
    if(!instance || !context || context->struct_size!=sizeof(*context) || (!bytes && length)) return ACESMS_FM_INVALID_ARGUMENT;
    if(context->master_clock_hz!=3579545 || context->native_divider!=72) return ACESMS_FM_UNSUPPORTED_CONFIG;
    return chip(instance)->loadState({bytes,length},context->master_ticks)?ACESMS_FM_OK:ACESMS_FM_INVALID_STATE;
}
const acesms_fm_api api{
    ACESMS_FM_ABI_VERSION,sizeof(acesms_fm_api),
    {{0x61,0x63,0x65,0x73,0x6d,0x73,0x2d,0x65,0x6d,0x75,0x32,0x34,0x31,0x33,0,1},
     "emu2413 1.6.4","a1d5120903b75f93d3eb7690e34a29c0b4add903",1,
     {0x65,0x6d,0x75,0x32,0x34,0x31,0x33,0x2d,0x6e,0x37,0x32,0x2d,0x76,0x31,0,1},3579545,72},
    create,destroy,reset,advanceTo,writeIo,stateSize,saveState,loadState
};
} // namespace
const acesms_fm_api* builtinFmApi(uint32_t requestedAbi) noexcept {return requestedAbi==ACESMS_FM_ABI_VERSION?&api:nullptr;}
} // namespace smsgg
