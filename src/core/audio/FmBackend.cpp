#include "core/audio/FmBackend.h"
#include "core/audio/BuiltinFm.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace smsgg {
namespace {
void requireSuccess(acesms_fm_result result,const char* operation) {
    if(result!=ACESMS_FM_OK) throw std::runtime_error(std::string("FM ")+operation+" failed: "+std::to_string(result));
}
}
std::variant<FmBackendPtr,std::string> FmBackendDefinition::fromApi(const acesms_fm_api* api,std::shared_ptr<void> module) {
    if(!api || api->abi_version!=ACESMS_FM_ABI_VERSION || api->struct_size!=sizeof(*api)) return std::string("Unsupported FM ABI/table size");
    if(!api->create || !api->destroy || !api->reset || !api->advance_to || !api->write_io || !api->state_size || !api->save_state || !api->load_state) return std::string("Missing FM operation");
    const auto& d=api->descriptor;
    if(d.master_clock_hz!=3579545 || d.native_divider!=72) return std::string("Unsupported FM clock/divider");
    if(!d.state_version || std::all_of(std::begin(d.backend_id),std::end(d.backend_id),[](uint8_t b){return b==0;}) ||
        std::all_of(std::begin(d.compatibility_id),std::end(d.compatibility_id),[](uint8_t b){return b==0;})) return std::string("Invalid FM state identity");
    if(!d.name[0] || !std::memchr(d.name,0,sizeof(d.name)) || !std::memchr(d.revision,0,sizeof(d.revision))) return std::string("Invalid FM descriptor text");
    return FmBackendPtr(new FmBackendDefinition(*api,std::move(module)));
}
FmBackendPtr FmBackendDefinition::builtin() {
    static const auto definition=std::get<FmBackendPtr>(fromApi(builtinFmApi(ACESMS_FM_ABI_VERSION)));
    return definition;
}
FmBackend::FmBackend(FmBackendPtr definition,uint32_t clockHz)
    : definition_(std::move(definition)),instance_(nullptr,definition_->api().destroy),clockHz_(clockHz) {
    const acesms_fm_config config{sizeof(acesms_fm_config),clockHz,72};
    acesms_fm_instance* instance=nullptr;
    const auto result=definition_->api().create(&config,&instance);
    instance_.reset(instance);
    requireSuccess(result,"create");
    if(!instance_) throw std::runtime_error("FM create returned no instance");
}
void FmBackend::reset() {
    requireSuccess(definition_->api().reset(instance_.get()),"reset");time_=0;
}
size_t FmBackend::advanceTo(uint64_t target,std::span<int32_t> output) {
    assert(target>=time_ && target/72-time_/72<=output.size() && output.size()<=std::numeric_limits<uint32_t>::max());
    uint32_t produced=0;
    requireSuccess(definition_->api().advance_to(instance_.get(),target,output.data(),static_cast<uint32_t>(output.size()),&produced),"advance");
    if(produced!=target/72-time_/72) throw std::runtime_error("FM advance returned an unexpected sample count");
    time_=target;return produced;
}
void FmBackend::writeIo(uint8_t port,uint8_t value) {
    requireSuccess(definition_->api().write_io(instance_.get(),port,value),"write");
}
uint32_t FmBackend::stateSize() const {
    uint32_t bytes=0;
    requireSuccess(definition_->api().state_size(instance_.get(),&bytes),"state_size");
    return bytes;
}
std::vector<uint8_t> FmBackend::saveState() const {
    std::vector<uint8_t> bytes(stateSize());uint32_t written=0;
    requireSuccess(definition_->api().save_state(instance_.get(),bytes.data(),static_cast<uint32_t>(bytes.size()),&written),"save");
    if(written!=bytes.size()) throw std::runtime_error("FM save returned an unexpected byte count");
    return bytes;
}
acesms_fm_result FmBackend::loadState(std::span<const uint8_t> bytes,uint64_t savedTicks) {
    if(bytes.size()>std::numeric_limits<uint32_t>::max()) return ACESMS_FM_INVALID_STATE;
    const acesms_fm_load_context context{sizeof(acesms_fm_load_context),clockHz_,savedTicks,72};
    const auto result=definition_->api().load_state(instance_.get(),bytes.data(),static_cast<uint32_t>(bytes.size()),&context);
    if(result==ACESMS_FM_OK) time_=savedTicks;
    return result;
}
} // namespace smsgg
