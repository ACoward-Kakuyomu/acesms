#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atlbase.h>
#include <atlapp.h>

#include <system_error>

namespace smsgg::win32 {
class WtlApplication final {
public:
    explicit WtlApplication(HINSTANCE instance) {
        const auto result = module_.Init(nullptr, instance);
        if (FAILED(result)) throw std::system_error(result, std::system_category(), "WTL CAppModule::Init");
    }
    ~WtlApplication() { module_.Term(); }
    WtlApplication(const WtlApplication&) = delete;
    WtlApplication& operator=(const WtlApplication&) = delete;
private:
    WTL::CAppModule module_;
};
} // namespace smsgg::win32
