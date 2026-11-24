#include "core/cartridge/RomLoader.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string_view>
#include <type_traits>

using smsgg::LoadOptions;
using smsgg::MapperKind;
using smsgg::RomImage;
using smsgg::RomLoader;
using smsgg::RomLoadError;

static_assert(!std::is_default_constructible_v<RomImage>);
static_assert(!std::is_aggregate_v<RomImage>);
static_assert(!std::is_constructible_v<RomImage, std::vector<uint8_t>, LoadOptions>);
static_assert(std::is_same_v<decltype(std::declval<RomImage&>().bytes()), std::span<const uint8_t>>);
static_assert(std::is_same_v<decltype(std::declval<RomImage&>().metadata()), const smsgg::RomMetadata&>);

namespace {
bool sizes() {
    constexpr std::array<size_t, 4> romOnly{8192, 16384, 32768, 49152};
    constexpr std::array<size_t, 9> sega{8192, 16384, 32768, 65536, 131072, 262144, 524288, 768 * 1024, 1048576};
    for (const auto mapper : {MapperKind::RomOnly, MapperKind::Sega}) {
        std::vector<size_t> sizes{0, 1, 511, 512, 4096, 12288, 24576, 98304, 1048577, 2097152};
        for (const auto size : {8192, 16384, 32768, 49152, 65536, 131072, 262144, 524288, 768 * 1024, 1048576}) {
            sizes.insert(sizes.end(), {size - 1u, static_cast<size_t>(size), size + 1u});
        }
        for (const auto size : sizes) {
            std::vector<uint8_t> bytes(size, 0x5a);
            if (!bytes.empty()) { bytes.front() = 0x12; bytes.back() = 0x34; }
            auto result = RomLoader::fromBytes(bytes, {mapper});
            const bool accepted = mapper == MapperKind::RomOnly
                ? std::find(romOnly.begin(), romOnly.end(), size) != romOnly.end()
                : std::find(sega.begin(), sega.end(), size) != sega.end();
            if (accepted) {
                const auto* image = std::get_if<RomImage>(&result);
                if (!image || image->bytes().size() != size || !std::equal(bytes.begin(), bytes.end(), image->bytes().begin())
                    || image->options().mapper != mapper || image->options().sramBytes != 0) return false;
            } else {
                const auto* error = std::get_if<RomLoadError>(&result);
                if (!error || *error != (size == 0 ? RomLoadError::EmptyInput : RomLoadError::UnsupportedRomSize)) {
                    std::cerr << "ROM size=" << size << " mapper=" << int(mapper) << '\n'; return false;
                }
            }
        }
    }
    const auto automatic = RomLoader::fromBytes(std::vector<uint8_t>(768 * 1024), {MapperKind::Auto});
    const auto* image = std::get_if<RomImage>(&automatic);
    if (!image || image->options().mapper != MapperKind::Sega ||
        image->metadata().mapperSelection != smsgg::MapperSelection::DefaultSega) return false;
    for (const auto options : {
        LoadOptions{MapperKind::Sega, 0, smsgg::CopierHeader::None, smsgg::Machine::GameGear},
        LoadOptions{MapperKind::Auto, 0, smsgg::CopierHeader::None, smsgg::Machine::GameGear},
        LoadOptions{MapperKind::RomOnly, 0, smsgg::CopierHeader::None, smsgg::Machine::Sg1000},
        LoadOptions{MapperKind::Codemasters}}) {
        const auto result = RomLoader::fromBytes(std::vector<uint8_t>(768 * 1024), options);
        const auto* error = std::get_if<RomLoadError>(&result);
        if (!error || *error != RomLoadError::UnsupportedRomSize) return false;
    }
    return true;
}

bool options() {
    for (const auto mapper : {MapperKind::RomOnly, MapperKind::Sega, static_cast<MapperKind>(255)}) {
        for (const size_t sram : {0, 1, 8192, 12288, 16384, 32768, 65536}) {
            const bool accepted = mapper == MapperKind::RomOnly ? sram == 0
                : mapper == MapperKind::Sega && (sram == 0 || sram == 8192 || sram == 16384 || sram == 32768);
            auto result = RomLoader::fromBytes(std::vector<uint8_t>(8192), {mapper, sram});
            if (accepted) {
                const auto* image = std::get_if<RomImage>(&result);
                if (!image || image->options().mapper != mapper || image->options().sramBytes != sram) return false;
            } else {
                const auto* error = std::get_if<RomLoadError>(&result);
                if (!error || *error != RomLoadError::IncompatibleOptions) return false;
            }
        }
    }
    return true;
}

bool prefix() {
    for (const auto mapper : {MapperKind::RomOnly, MapperKind::Sega}) {
        for (const size_t size : {8192, 16384, 32768, 49152, 65536, 131072, 262144, 524288, 768 * 1024, 1048576}) {
            const bool supported = mapper == MapperKind::Sega ? size != 49152 : size <= 49152;
            std::vector<uint8_t> raw(size + 512, 0xa5);
            for (size_t i = 0; i < size; ++i) raw[i + 512] = static_cast<uint8_t>(i ^ (i / 256));
            auto plain = RomLoader::fromBytes(raw, {mapper});
            const auto* plainError = std::get_if<RomLoadError>(&plain);
            if (!plainError || *plainError != RomLoadError::UnsupportedRomSize) return false;
            auto stripped = RomLoader::fromBytes(raw, {mapper, 0, smsgg::CopierHeader::Strip512});
            if (supported) {
                const auto* image = std::get_if<RomImage>(&stripped);
                if (!image || image->bytes().size() != size
                    || image->options().copierHeader != smsgg::CopierHeader::Strip512
                    || !std::equal(raw.begin() + 512, raw.end(), image->bytes().begin())) return false;
            } else {
                const auto* error = std::get_if<RomLoadError>(&stripped);
                if (!error || *error != RomLoadError::UnsupportedRomSize) return false;
            }
        }
    }
    for (const size_t size : {0, 1, 511, 512, 513, 8703, 8705, 1049089}) {
        auto result = RomLoader::fromBytes(std::vector<uint8_t>(size), {MapperKind::Sega, 0, smsgg::CopierHeader::Strip512});
        const auto* error = std::get_if<RomLoadError>(&result);
        const auto expected = size == 0 ? RomLoadError::EmptyInput : size <= 512 ? RomLoadError::InvalidCopierHeader
            : RomLoadError::UnsupportedRomSize;
        if (!error || *error != expected) return false;
    }
    std::vector<uint8_t> raw(8192, 0xa5);
    auto plain = RomLoader::fromBytes(raw, {MapperKind::RomOnly});
    const auto* image = std::get_if<RomImage>(&plain);
    if (!image || image->bytes().size() != raw.size() || image->bytes().front() != 0xa5) return false;
    auto invalid = RomLoader::fromBytes(raw, {MapperKind::Sega, 0, static_cast<smsgg::CopierHeader>(255)});
    const auto* error = std::get_if<RomLoadError>(&invalid);
    return error && *error == RomLoadError::IncompatibleOptions;
}

void writeHeader(std::vector<uint8_t>& bytes, size_t offset, uint8_t region, uint8_t sizeCode) {
    constexpr std::array<uint8_t, 8> signature{'T', 'M', 'R', ' ', 'S', 'E', 'G', 'A'};
    std::copy(signature.begin(), signature.end(), bytes.begin() + offset);
    bytes[offset + 10] = 0x12; bytes[offset + 11] = 0x34;
    bytes[offset + 15] = static_cast<uint8_t>(region * 16 + sizeCode);
}

bool sg1000() {
    const LoadOptions options{.mapper = MapperKind::RomOnly, .machine = smsgg::Machine::Sg1000};
    for (const size_t size : {8192, 16384, 32768, 40960, 49152}) {
        std::vector<uint8_t> bytes(size);
        writeHeader(bytes, 0x1ff0, 7, 0xa);
        const auto result = RomLoader::fromBytes(std::move(bytes), options);
        const auto* image = std::get_if<RomImage>(&result);
        if (!image || image->bytes().size() != size || image->options().machine != smsgg::Machine::Sg1000
            || !image->metadata().header || image->metadata().header->model != smsgg::RomModel::GameGear) return false;
    }
    const auto sms = RomLoader::fromBytes(std::vector<uint8_t>(40960), {MapperKind::RomOnly});
    const auto* smsError = std::get_if<RomLoadError>(&sms);
    if (!smsError || *smsError != RomLoadError::UnsupportedRomSize) return false;
    for (const size_t size : {8191, 8193, 24576, 40959, 40961, 65536}) {
        const auto result = RomLoader::fromBytes(std::vector<uint8_t>(size), options);
        const auto* error = std::get_if<RomLoadError>(&result);
        if (!error || *error != RomLoadError::UnsupportedRomSize) return false;
    }
    for (const auto mapper : {MapperKind::Sega, MapperKind::Auto, MapperKind::Codemasters}) {
        auto incompatible = options;
        incompatible.mapper = mapper;
        const auto result = RomLoader::fromBytes(std::vector<uint8_t>(32768), incompatible);
        const auto* error = std::get_if<RomLoadError>(&result);
        if (!error || *error != RomLoadError::IncompatibleOptions) return false;
    }
    auto ram = options;
    ram.sramBytes = 8192;
    const auto result = RomLoader::fromBytes(std::vector<uint8_t>(32768), ram);
    const auto* error = std::get_if<RomLoadError>(&result);
    return error && *error == RomLoadError::IncompatibleOptions;
}

bool header() {
    constexpr std::array<size_t, 16> declared{
        262144, 524288, 1048576, 0, 0, 0, 0, 0, 0, 0, 8192, 16384, 32768, 49152, 65536, 131072};
    for (unsigned region = 0; region < 16; ++region) {
        for (unsigned code = 0; code < 16; ++code) {
            std::vector<uint8_t> bytes(32768);
            writeHeader(bytes, 0x7ff0, static_cast<uint8_t>(region), static_cast<uint8_t>(code));
            auto result = RomLoader::fromBytes(bytes, {MapperKind::RomOnly});
            const auto* image = std::get_if<RomImage>(&result);
            if (!image || !image->metadata().header || image->metadata().size != bytes.size()) return false;
            const auto& h = *image->metadata().header;
            const auto model = region == 3 || region == 4 ? smsgg::RomModel::Sms
                : region >= 5 && region <= 7 ? smsgg::RomModel::GameGear : smsgg::RomModel::Unknown;
            if (h.offset != 0x7ff0 || h.checksum != 0x3412 || h.regionCode != region || h.sizeCode != code
                || h.model != model || h.declaredSize.has_value() != (declared[code] != 0)
                || (h.declaredSize && *h.declaredSize != declared[code])
                || !std::equal(bytes.begin(), bytes.end(), image->bytes().begin())) return false;
        }
    }
    for (const size_t size : {8192, 16384, 32768}) {
        for (const size_t offset : {0x1ff0, 0x3ff0, 0x7ff0}) {
            std::vector<uint8_t> bytes(size);
            const bool fits = offset + 16 <= size;
            if (fits) writeHeader(bytes, offset, 4, 0);
            auto result = RomLoader::fromBytes(std::move(bytes), {MapperKind::RomOnly});
            const auto* image = std::get_if<RomImage>(&result);
            if (!image || image->metadata().header.has_value() != fits
                || (fits && image->metadata().header->offset != offset)) return false;
        }
    }
    std::vector<uint8_t> bytes(32768);
    for (const size_t offset : {0x1ff0, 0x3ff0, 0x7ff0}) writeHeader(bytes, offset, 4, 0);
    for (const size_t offset : {0x7ff0, 0x3ff0, 0x1ff0}) {
        auto result = RomLoader::fromBytes(bytes, {MapperKind::Sega});
        const auto* image = std::get_if<RomImage>(&result);
        if (!image || !image->metadata().header || image->metadata().header->offset != offset) return false;
        bytes[offset + 7] ^= 1;
    }
    auto absent = RomLoader::fromBytes(std::move(bytes), {MapperKind::Sega});
    if (!std::holds_alternative<RomImage>(absent) || std::get<RomImage>(absent).metadata().header) return false;
    std::vector<uint8_t> prefixed(32768 + 512, 0xa5);
    writeHeader(prefixed, 512 + 0x7ff0, 7, 0xa);
    auto stripped = RomLoader::fromBytes(std::move(prefixed), {MapperKind::Sega, 0, smsgg::CopierHeader::Strip512});
    const auto* normalized = std::get_if<RomImage>(&stripped);
    return normalized && normalized->metadata().header && normalized->metadata().header->offset == 0x7ff0
        && normalized->metadata().header->model == smsgg::RomModel::GameGear
        && normalized->options().mapper == MapperKind::Sega && normalized->bytes().size() == 32768;
}

bool crc() {
    struct Vector { size_t size; uint32_t zeros, sequence; };
    constexpr std::array<Vector, 3> vectors{{
        {8192, 0xd8f49994, 0xb6675307}, {16384, 0xab54d286, 0xe81722f0}, {32768, 0x011ffca6, 0x217726b2},
    }};
    for (const auto& vector : vectors) {
        for (const bool sequence : {false, true}) {
            for (const size_t prefix : {0, 512}) {
                std::vector<uint8_t> bytes(vector.size + prefix, 0xa5);
                for (size_t i = 0; i < vector.size; ++i) bytes[prefix + i] = sequence ? static_cast<uint8_t>(i) : 0;
                auto result = RomLoader::fromBytes(std::move(bytes), {MapperKind::Sega, 0,
                    prefix == 0 ? smsgg::CopierHeader::None : smsgg::CopierHeader::Strip512});
                const auto* image = std::get_if<RomImage>(&result);
                if (!image || image->metadata().size != vector.size || image->metadata().header
                    || image->metadata().crc32 != (sequence ? vector.sequence : vector.zeros)) return false;
            }
        }
    }
    return true;
}

bool file(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return false;
    const auto path = folder / std::filesystem::path(L"loader-\u30ed\u30fc\u30c9.sms");
    for (const size_t size : {8192, 32768, 49152, 768 * 1024, 1048576}) {
        for (const size_t prefix : {0, 512}) {
            const LoadOptions options{size == 49152 ? MapperKind::RomOnly : MapperKind::Sega,
                size == 49152 ? 0u : 32768u, prefix == 0 ? smsgg::CopierHeader::None : smsgg::CopierHeader::Strip512};
            std::vector<uint8_t> raw(size + prefix);
            for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint8_t>(i ^ (i / 256));
            writeHeader(raw, prefix + 0x1ff0, 4, 0xa);
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            output.close(); if (!output) return false;
            auto direct = RomLoader::fromBytes(raw, options), loaded = RomLoader::load(path, options);
            const auto* a = std::get_if<RomImage>(&direct); const auto* b = std::get_if<RomImage>(&loaded);
            if (!a || !b || a->bytes().size() != b->bytes().size()
                || !std::equal(a->bytes().begin(), a->bytes().end(), b->bytes().begin())
                || a->metadata().size != b->metadata().size || a->metadata().crc32 != b->metadata().crc32
                || !b->metadata().header || b->metadata().header->offset != 0x1ff0
                || b->metadata().header->checksum != 0x3412 || b->metadata().header->regionCode != 4
                || b->options().mapper != options.mapper || b->options().sramBytes != options.sramBytes
                || b->options().copierHeader != options.copierHeader) return false;
        }
    }
    for (const size_t size : {0, 1, 512, 8193, 1048577}) {
        std::vector<uint8_t> raw(size);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!raw.empty()) output.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        output.close(); if (!output) return false;
        auto loaded = RomLoader::load(path, {MapperKind::Sega});
        const auto* result = std::get_if<RomLoadError>(&loaded);
        if (!result || *result != (size == 0 ? RomLoadError::EmptyInput : RomLoadError::UnsupportedRomSize)) return false;
        if (size == 512) {
            auto stripped = RomLoader::load(path, {MapperKind::Sega, 0, smsgg::CopierHeader::Strip512});
            const auto* prefixError = std::get_if<RomLoadError>(&stripped);
            if (!prefixError || *prefixError != RomLoadError::InvalidCopierHeader) return false;
        }
    }
    auto incompatible = RomLoader::load(path, {MapperKind::RomOnly, 8192});
    const auto* optionsError = std::get_if<RomLoadError>(&incompatible);
    if (!optionsError || *optionsError != RomLoadError::IncompatibleOptions) return false;
    auto missing = RomLoader::load(folder / "never-created" / "absent.sms", {MapperKind::Sega});
    const auto* result = std::get_if<RomLoadError>(&missing);
    return result && *result == RomLoadError::OpenFailed;
}

bool ownership() {
    std::vector<uint8_t> source(8192);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i);
    auto result = RomLoader::fromBytes(source, {MapperKind::RomOnly});
    auto* image = std::get_if<RomImage>(&result);
    if (!image) return false;
    source.assign(8192, 0xff);
    for (size_t i = 0; i < image->bytes().size(); ++i) {
        if (image->bytes()[i] != static_cast<uint8_t>(i)) return false;
    }
    auto other = RomLoader::fromBytes(std::move(source), {MapperKind::Sega, 32768});
    const auto* second = std::get_if<RomImage>(&other);
    return second && second->bytes().size() == 8192 && second->bytes()[0] == 0xff && image->bytes()[0] == 0;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "file") return file(std::filesystem::path(argv[2])) ? 0 : 1;
    if (argc == 2) {
        const std::string_view name(argv[1]);
        if (name == "sizes") return sizes() ? 0 : 1;
        if (name == "options") return options() ? 0 : 1;
        if (name == "ownership") return ownership() ? 0 : 1;
        if (name == "prefix") return prefix() ? 0 : 1;
        if (name == "header") return header() ? 0 : 1;
        if (name == "crc") return crc() ? 0 : 1;
        if (name == "sg1000") return sg1000() ? 0 : 1;
    }
    std::cerr << "Failed ROM loader test\n";
    return 1;
}
