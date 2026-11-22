#pragma once

#include <filesystem>
#include <string>
#include <variant>

namespace smsgg::win32 {
std::variant<std::filesystem::path,std::string> resolveDatabasePath(const std::filesystem::path& explicitPath);
}
