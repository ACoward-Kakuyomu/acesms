#include "win32/FileStore.h"

#include <objbase.h>

#include <algorithm>
#include <memory>

namespace smsgg::win32 {
std::variant<std::vector<uint8_t>,FileError> readFileExact(const std::filesystem::path& path,size_t expectedBytes,const FileIo& io) {
    const auto raw=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(raw==INVALID_HANDLE_VALUE) return FileError{FileOperation::Open,GetLastError()};
    std::unique_ptr<void,decltype(&::CloseHandle)> handle(raw,::CloseHandle);
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(raw,&size)) return FileError{FileOperation::Size,GetLastError()};
    if(size.QuadPart<0 || static_cast<uint64_t>(size.QuadPart)!=expectedBytes)
        return FileError{FileOperation::Size,ERROR_BAD_LENGTH};
    std::vector<uint8_t> bytes(expectedBytes);size_t at=0;
    while(at<bytes.size()) {
        const auto count=static_cast<DWORD>(std::min<size_t>(bytes.size()-at,MAXDWORD));DWORD done=0;
        if(!io.read(raw,bytes.data()+at,count,&done,nullptr)) return FileError{FileOperation::Read,GetLastError()};
        if(!done) return FileError{FileOperation::Read,ERROR_HANDLE_EOF};
        at+=done;
    }
    if(!io.close(handle.release())) return FileError{FileOperation::Close,GetLastError()};
    return bytes;
}
std::optional<FileError> writeFileAtomically(const std::filesystem::path& path,std::span<const uint8_t> bytes,const FileIo& io) {
    GUID guid{};
    const auto result = CoCreateGuid(&guid);
    if (FAILED(result)) return FileError{FileOperation::Open, static_cast<DWORD>(result)};
    wchar_t name[39]{}; // Braced GUID and terminating null.
    if (!StringFromGUID2(guid, name, static_cast<int>(std::size(name))))
        return FileError{FileOperation::Open, ERROR_INSUFFICIENT_BUFFER};
    const auto temporary = path.parent_path() / (std::wstring(L".acesms-") + name + L".tmp");
    const auto raw=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(raw==INVALID_HANDLE_VALUE) return FileError{FileOperation::Open,GetLastError()};
    std::unique_ptr<void,decltype(&::CloseHandle)> handle(raw,::CloseHandle);
    auto fail=[&](FileOperation operation,DWORD code)->std::optional<FileError> {
        handle.reset();
        const auto cleanup=DeleteFileW(temporary.c_str())?ERROR_SUCCESS:GetLastError();
        return FileError{operation,code,cleanup};
    };
    size_t at=0;
    while(at<bytes.size()) {
        const auto count=static_cast<DWORD>(std::min<size_t>(bytes.size()-at,MAXDWORD));DWORD done=0;
        if(!io.write(raw,bytes.data()+at,count,&done,nullptr)) return fail(FileOperation::Write,GetLastError());
        if(!done) return fail(FileOperation::Write,ERROR_WRITE_FAULT);
        at+=done;
    }
    if(!io.flush(raw)) return fail(FileOperation::Flush,GetLastError());
    if(!io.close(handle.release())) return fail(FileOperation::Close,GetLastError());
    if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return fail(FileOperation::Replace,GetLastError());
    return std::nullopt;
}
}
