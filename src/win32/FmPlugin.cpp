#include "win32/FmPlugin.h"
#include <Windows.h>
#include <array>
#include <system_error>

namespace smsgg {
std::filesystem::path fmPluginDirectory() {
    std::array<wchar_t,32768> executable{};
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(!length || length==executable.size()) throw std::system_error(GetLastError(),std::system_category(),"Determine FM plugin directory");
    return std::filesystem::path(executable.data()).parent_path()/L"plugins"/L"fm";
}
std::variant<FmBackendPtr,std::string> loadFmPlugin(const std::filesystem::path& directory,const std::filesystem::path& filename) {
    if(filename.empty() || filename.has_parent_path() || _wcsicmp(filename.extension().c_str(),L".dll")) return std::string("FM plugin must be a single DLL filename");
    std::error_code error;
    auto path=std::filesystem::absolute(directory/filename,error);
    if(error) return std::string("FM plugin path: ")+error.message();
    path.make_preferred();
    DWORD previousMode=0;
    if(!SetThreadErrorMode(GetThreadErrorMode()|SEM_FAILCRITICALERRORS,&previousMode)) return std::string("FM plugin error mode failed: Win32 ")+std::to_string(GetLastError());
    const auto handle=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto loadError=GetLastError();
    if(!SetThreadErrorMode(previousMode,nullptr)) {
        const auto restoreError=GetLastError();
        if(handle) FreeLibrary(handle);
        return std::string("FM plugin error mode restore failed: Win32 ")+std::to_string(restoreError);
    }
    if(!handle) return std::string("FM plugin load failed: Win32 ")+std::to_string(loadError);
    std::shared_ptr<void> module(handle,[](void* value){FreeLibrary(static_cast<HMODULE>(value));});
    const auto getApi=reinterpret_cast<acesms_fm_get_api_fn>(GetProcAddress(handle,"acesms_fm_get_api"));
    if(!getApi) return std::string("FM plugin entry acesms_fm_get_api is missing");
    return FmBackendDefinition::fromApi(getApi(ACESMS_FM_ABI_VERSION),std::move(module));
}
}
