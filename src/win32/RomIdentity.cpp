#include "win32/RomDatabase.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

namespace smsgg::win32 {
std::variant<RomIdentity,DatabaseError> identifyRom(const RomImage& image) {
    RomIdentity identity;
    identity.bytes=static_cast<uint32_t>(image.bytes().size());
    identity.crc32=image.metadata().crc32;
    const auto input=const_cast<PUCHAR>(image.bytes().data());
    auto status=BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,input,identity.bytes,identity.sha256.data(),static_cast<ULONG>(identity.sha256.size()));
    if(status<0) return DatabaseError{DatabaseErrorKind::Hash,static_cast<int>(status),"SHA-256 failed"};
    status=BCryptHash(BCRYPT_SHA1_ALG_HANDLE,nullptr,0,input,identity.bytes,identity.sha1.data(),static_cast<ULONG>(identity.sha1.size()));
    if(status<0) return DatabaseError{DatabaseErrorKind::Hash,static_cast<int>(status),"SHA-1 failed"};
    return identity;
}
}
