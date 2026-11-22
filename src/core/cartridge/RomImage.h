#pragma once

#include "core/Machine.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <utility>
#include <vector>

namespace smsgg {

enum class MapperKind { RomOnly, Sega, Codemasters, Auto };
enum class CopierHeader { None, Strip512 };

constexpr bool supportsLinearRomSize(uintmax_t size, Machine machine) {
    return size == 8192 || size == 16384 || size == 32768 || size == 49152
        || (machine == Machine::Sg1000 && size == 40960);
}

struct LoadOptions {
    MapperKind mapper;
    size_t sramBytes{};
    CopierHeader copierHeader{CopierHeader::None};
    Machine machine{Machine::Sms};
};

enum class RomModel { Unknown, Sms, GameGear };

struct SegaHeader {
    size_t offset;
    uint16_t checksum;
    uint8_t regionCode;
    uint8_t sizeCode;
    RomModel model;
    std::optional<size_t> declaredSize;
};

enum class ChecksumStatus { Mismatch, Match };

struct CodemastersHeader {
    uint8_t pages;
    uint16_t checksum;
    uint16_t computedChecksum;
    ChecksumStatus status;
};

enum class MapperSelection { Explicit, CodemastersHeader, Linear48K, DefaultSega };

struct RomMetadata {
    size_t size;
    uint32_t crc32;
    std::optional<SegaHeader> header;
    std::optional<CodemastersHeader> codemasters;
    MapperSelection mapperSelection{MapperSelection::Explicit};
};

class RomImage final {
public:
    std::span<const uint8_t> bytes() const { return bytes_; }
    const LoadOptions& options() const { return options_; }
    const RomMetadata& metadata() const { return metadata_; }
private:
    friend class RomLoader;
    RomImage(std::vector<uint8_t> bytes, LoadOptions options, RomMetadata metadata)
        : bytes_(std::move(bytes)), options_(options), metadata_(metadata) {}
    std::vector<uint8_t> bytes_;
    LoadOptions options_;
    RomMetadata metadata_;
};

} // namespace smsgg
