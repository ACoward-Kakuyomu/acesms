#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace smsgg::win32 {
enum class FileOperation {Open,Size,Read,Write,Flush,Close,Replace};
struct FileError {FileOperation operation;DWORD code;DWORD cleanupCode{};};
struct FileIo {
    decltype(&::ReadFile) read{::ReadFile};
    decltype(&::WriteFile) write{::WriteFile};
    decltype(&::FlushFileBuffers) flush{::FlushFileBuffers};
    decltype(&::CloseHandle) close{::CloseHandle};
};
std::variant<std::vector<uint8_t>,FileError> readFileExact(const std::filesystem::path& path,size_t expectedBytes,const FileIo& io={});
std::optional<FileError> writeFileAtomically(const std::filesystem::path& path,std::span<const uint8_t> bytes,const FileIo& io={});
}
