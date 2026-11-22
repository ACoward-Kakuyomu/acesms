#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace smsgg::test {
inline std::vector<uint8_t> codemastersHeaderFixture(size_t size=32768) {
    std::vector<uint8_t> bytes(size);
    bytes[0x7fe0]=static_cast<uint8_t>(size/16384);
    bytes[0x7fe1]=1;bytes[0x7fe2]=1;bytes[0x7fe3]=0x94;
    // Independent fixed arithmetic for a zero payload and this exact date.
    const auto sum=static_cast<uint16_t>(0x9501+size/16384);
    const auto complement=static_cast<uint16_t>(0u-sum);
    bytes[0x7fe6]=static_cast<uint8_t>(sum);bytes[0x7fe7]=static_cast<uint8_t>(sum>>8);
    bytes[0x7fe8]=static_cast<uint8_t>(complement);bytes[0x7fe9]=static_cast<uint8_t>(complement>>8);
    return bytes;
}
} // namespace smsgg::test
