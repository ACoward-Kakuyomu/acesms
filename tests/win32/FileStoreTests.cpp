#include "win32/FileStore.h"

#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg::win32;
std::vector<uint8_t> disk(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
void seed(const std::filesystem::path& p,std::span<const uint8_t> b) {std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));}
constexpr std::array<uint8_t,4> old{1,3,5,7},fresh{2,4,6,8};
BOOL WINAPI shortRead(HANDLE h,LPVOID p,DWORD n,LPDWORD done,LPOVERLAPPED ov) {return ReadFile(h,p,std::min<DWORD>(n,17),done,ov);}
BOOL WINAPI shortWrite(HANDLE h,LPCVOID p,DWORD n,LPDWORD done,LPOVERLAPPED ov) {return WriteFile(h,p,std::min<DWORD>(n,13),done,ov);}
BOOL WINAPI failRead(HANDLE,LPVOID,DWORD,LPDWORD done,LPOVERLAPPED) {*done=0;SetLastError(ERROR_READ_FAULT);return FALSE;}
BOOL WINAPI zeroRead(HANDLE,LPVOID,DWORD,LPDWORD done,LPOVERLAPPED) {*done=0;return TRUE;}
BOOL WINAPI failWrite(HANDLE,LPCVOID,DWORD,LPDWORD done,LPOVERLAPPED) {*done=0;SetLastError(ERROR_DISK_FULL);return FALSE;}
BOOL WINAPI zeroWrite(HANDLE,LPCVOID,DWORD,LPDWORD done,LPOVERLAPPED) {*done=0;return TRUE;}
BOOL WINAPI failFlush(HANDLE) {SetLastError(ERROR_WRITE_FAULT);return FALSE;}
BOOL WINAPI failClose(HANDLE h) {CloseHandle(h);SetLastError(ERROR_INVALID_HANDLE);return FALSE;}
std::filesystem::path heldDirectory, heldPath;
HANDLE held=INVALID_HANDLE_VALUE;
BOOL WINAPI lockOnClose(HANDLE h) {
    CloseHandle(h);
    for (const auto& entry : std::filesystem::directory_iterator(heldDirectory)) {
        const auto& path = entry.path();
        if (path.filename().native().starts_with(L".acesms-") && path.extension() == L".tmp") {
            heldPath = path;
            held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            break;
        }
    }
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
bool read(const std::filesystem::path& dir) {
    const auto path=dir/L"\u8aad\u307f\u8fbc\u307f.bin";seed(path,old);
    const auto bytes=readFileExact(path,4);
    if(!std::holds_alternative<std::vector<uint8_t>>(bytes) || std::get<std::vector<uint8_t>>(bytes)!=std::vector<uint8_t>(old.begin(),old.end())) return false;
    for(size_t size:{3u,5u}) {auto result=readFileExact(path,size);if(!std::holds_alternative<FileError>(result) || std::get<FileError>(result).operation!=FileOperation::Size) return false;}
    auto missing=readFileExact(dir/L"missing.bin",4);
    if(!std::holds_alternative<FileError>(missing) || std::get<FileError>(missing).code!=ERROR_FILE_NOT_FOUND) return false;
    for(auto fn:{failRead,zeroRead}) {FileIo io;io.read=fn;const auto result=readFileExact(path,4,io);if(!std::holds_alternative<FileError>(result) || std::get<FileError>(result).operation!=FileOperation::Read) return false;}
    FileIo io;io.close=failClose;const auto result=readFileExact(path,4,io);
    return std::holds_alternative<FileError>(result) && std::get<FileError>(result).operation==FileOperation::Close;
}
bool replace(const std::filesystem::path& dir) {
    const auto path=dir/L"\u7f6e\u63db.state";
    if(writeFileAtomically(path,old) || writeFileAtomically(path,fresh) || disk(path)!=std::vector<uint8_t>(fresh.begin(),fresh.end())) return false;
    std::vector<uint8_t> large(100003);for(size_t n=0;n<large.size();++n) large[n]=static_cast<uint8_t>(n*37);
    FileIo io;io.read=shortRead;io.write=shortWrite;
    if(writeFileAtomically(path,large,io)) return false;
    const auto result=readFileExact(path,large.size(),io);
    if(!std::holds_alternative<std::vector<uint8_t>>(result) || std::get<std::vector<uint8_t>>(result)!=large) return false;
    return !writeFileAtomically(path,{}) && std::get<std::vector<uint8_t>>(readFileExact(path,0)).empty();
}
bool failures(const std::filesystem::path& dir) {
    const auto path=dir/L"preserved.bin";seed(path,old);
    for(unsigned n=0;n<4;++n) {
        FileIo io;const auto expected=n<2?FileOperation::Write:n==2?FileOperation::Flush:FileOperation::Close;
        if(n==0) io.write=failWrite;if(n==1) io.write=zeroWrite;if(n==2) io.flush=failFlush;if(n==3) io.close=failClose;
        const auto result=writeFileAtomically(path,fresh,io);
        const bool leftover = std::ranges::any_of(std::filesystem::directory_iterator(dir), [](const auto& entry) {
            return entry.path().extension() == L".tmp";
        });
        if(!result || result->operation!=expected || result->cleanupCode || disk(path)!=std::vector<uint8_t>(old.begin(),old.end()) || leftover) return false;
    }
    HANDLE locked=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(locked==INVALID_HANDLE_VALUE) return false;
    const auto blocked=writeFileAtomically(path,fresh);CloseHandle(locked);
    if(!blocked || blocked->operation!=FileOperation::Replace || disk(path)!=std::vector<uint8_t>(old.begin(),old.end())) return false;
    if(!SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY)) return false;
    const auto readonly=writeFileAtomically(path,fresh);SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);
    return readonly && readonly->operation==FileOperation::Replace && disk(path)==std::vector<uint8_t>(old.begin(),old.end());
}
bool temporary(const std::filesystem::path& dir) {
    const auto path=dir/L"target.bin";const auto tmp=std::filesystem::path(path.wstring()+L".tmp");seed(path,old);seed(tmp,old);
    const auto result=writeFileAtomically(path,fresh);
    const bool kept=!result && disk(tmp)==std::vector<uint8_t>(old.begin(),old.end()) && disk(path)==std::vector<uint8_t>(fresh.begin(),fresh.end());
    const auto absent=writeFileAtomically(dir/L"absent"/L"target",fresh);
    heldDirectory=dir;FileIo io;io.close=lockOnClose;const auto cleanup=writeFileAtomically(path,old,io);
    const bool reported=held!=INVALID_HANDLE_VALUE && cleanup && cleanup->operation==FileOperation::Close &&
        cleanup->cleanupCode==ERROR_SHARING_VIOLATION && std::filesystem::exists(heldPath) &&
        disk(path)==std::vector<uint8_t>(fresh.begin(),fresh.end()) && disk(tmp)==std::vector<uint8_t>(old.begin(),old.end());
    if(held!=INVALID_HANDLE_VALUE) {CloseHandle(held);held=INVALID_HANDLE_VALUE;std::filesystem::remove(heldPath);}
    std::filesystem::remove(tmp);
    if(!reported) std::cerr << "Temporary cleanup error was not reported: " << (cleanup ? cleanup->cleanupCode : 0) << '\n';
    return kept && absent && absent->operation==FileOperation::Open && reported;
}
}
int main(int argc,char** argv) {
    if(argc!=3) return 1;const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);
    const std::string_view op=argv[1];const bool pass=op=="read"?read(dir):op=="replace"?replace(dir):op=="failures"?failures(dir):op=="temporary"&&temporary(dir);
    if(!pass) std::cerr<<"File store failed: "<<op<<'\n';return pass?0:1;
}
