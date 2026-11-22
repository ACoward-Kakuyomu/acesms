#pragma once

#include "core/audio/FmBackend.h"
#include <filesystem>

namespace smsgg {
std::filesystem::path fmPluginDirectory();
std::variant<FmBackendPtr,std::string> loadFmPlugin(const std::filesystem::path& directory,const std::filesystem::path& filename);
}
