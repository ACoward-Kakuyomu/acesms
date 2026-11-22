#include "win32/DatabasePath.h"

#include <Windows.h>
#include <array>

namespace smsgg::win32 {
std::variant<std::filesystem::path,std::string> resolveDatabasePath(const std::filesystem::path& explicitPath) {
    if(!explicitPath.empty()) {
        if(!explicitPath.is_absolute()) return std::string("Database path must be absolute");
        return explicitPath;
    }
    std::array<wchar_t,32768> executable{};
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(!length || length==executable.size())
        return "Cannot resolve executable path (Win32 "+std::to_string(GetLastError())+")";
    return std::filesystem::path(executable.data()).parent_path()/L"acesms.sqlite3";
}
}
