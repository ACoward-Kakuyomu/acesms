#pragma once

#include "acesms_fm.h"

#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace smsgg {
class FmBackendDefinition;
using FmBackendPtr=std::shared_ptr<const FmBackendDefinition>;

class FmBackendDefinition final {
public:
    static std::variant<FmBackendPtr,std::string> fromApi(const acesms_fm_api* api,std::shared_ptr<void> module={});
    static FmBackendPtr builtin();
    const acesms_fm_api& api() const noexcept {return api_;}
private:
    FmBackendDefinition(const acesms_fm_api& api,std::shared_ptr<void> module) : api_(api),module_(std::move(module)) {}
    acesms_fm_api api_;
    std::shared_ptr<void> module_;
};

// Definition outlives the instance, including the creating module's destroy callback.
class FmBackend final {
public:
    FmBackend(FmBackendPtr definition,uint32_t clockHz);
    FmBackend(const FmBackend&)=delete;
    FmBackend& operator=(const FmBackend&)=delete;
    void reset();
    size_t advanceTo(uint64_t target,std::span<int32_t> output);
    void writeIo(uint8_t port,uint8_t value);
    uint64_t masterTicks() const noexcept {return time_;}
    uint32_t stateSize() const;
    std::vector<uint8_t> saveState() const;
    acesms_fm_result loadState(std::span<const uint8_t> bytes,uint64_t savedTicks);
private:
    FmBackendPtr definition_;
    std::unique_ptr<acesms_fm_instance,decltype(acesms_fm_api::destroy)> instance_;
    uint32_t clockHz_;
    uint64_t time_{};
};
} // namespace smsgg
