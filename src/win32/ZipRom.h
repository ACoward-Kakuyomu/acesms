#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace smsgg::win32 {
struct ZipRom {
    std::filesystem::path entry;
    std::vector<uint8_t> bytes;
};
bool isRawRomPath(const std::filesystem::path& path);
bool isZipRomPath(const std::filesystem::path& path);
std::variant<ZipRom,std::string> readZipRom(const std::filesystem::path& path);
std::string zipRomError(const std::filesystem::path& path,const std::filesystem::path& entry,std::string_view reason);
}
