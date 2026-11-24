#include "core/cartridge/RomLoader.h"

#include <bit>
#include <array>
#include <algorithm>
#include <fstream>

namespace smsgg {

namespace {
std::optional<RomLoadError> validate(uintmax_t rawSize, LoadOptions options) {
    if (rawSize == 0) return RomLoadError::EmptyInput;
    const bool romOnly = options.mapper == MapperKind::RomOnly;
    const bool sg1000 = options.machine == Machine::Sg1000;
    const bool codemasters = options.mapper == MapperKind::Codemasters;
    const bool automatic = options.mapper == MapperKind::Auto;
    if ((options.copierHeader != CopierHeader::None && options.copierHeader != CopierHeader::Strip512)
        || (!romOnly && !codemasters && !automatic && options.mapper != MapperKind::Sega)
        || (sg1000 && !romOnly)
        || (romOnly && options.sramBytes != 0)
        || (codemasters && options.sramBytes != 0 && options.sramBytes != 8192)
        || (options.sramBytes != 0 && options.sramBytes != 8192
            && options.sramBytes != 16384 && options.sramBytes != 32768)) return RomLoadError::IncompatibleOptions;
    const size_t prefix = options.copierHeader == CopierHeader::Strip512 ? 512 : 0;
    if (prefix != 0 && rawSize <= prefix) return RomLoadError::InvalidCopierHeader;
    const auto size = rawSize - prefix;
    const bool supported = romOnly ? supportsLinearRomSize(size, options.machine)
        : codemasters ? size >= 32768 && size <= 1048576 && std::has_single_bit(size)
        : (automatic && size == 49152) || supportsSegaRomSize(size, options.machine);
    if (!supported) return RomLoadError::UnsupportedRomSize;
    return std::nullopt;
}
} // namespace

std::variant<RomImage, RomLoadError> RomLoader::fromBytes(std::vector<uint8_t> bytes, LoadOptions options) {
    if (const auto error = validate(bytes.size(), options)) return *error;
    return finish(std::move(bytes), options);
}

std::variant<RomImage, RomLoadError> RomLoader::load(const std::filesystem::path& path, LoadOptions options) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open()) return RomLoadError::OpenFailed;
    const std::streamoff size = input.tellg();
    if (size < 0) return RomLoadError::SizeFailed;
    if (const auto error = validate(static_cast<uintmax_t>(size), options)) return *error;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!input.seekg(0) || !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return RomLoadError::ReadFailed;
    }
    return finish(std::move(bytes), options);
}

std::variant<RomImage, RomLoadError> RomLoader::finish(std::vector<uint8_t> bytes, LoadOptions options) {
    const size_t prefix = options.copierHeader == CopierHeader::Strip512 ? 512 : 0;
    if (prefix != 0) bytes.erase(bytes.begin(), bytes.begin() + prefix);
    const auto size = bytes.size();
    uint32_t crc = 0xffffffff;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320 : 0);
    }
    RomMetadata metadata{size, crc ^ 0xffffffff, std::nullopt};
    constexpr std::array<uint8_t, 8> signature{'T', 'M', 'R', ' ', 'S', 'E', 'G', 'A'};
    constexpr std::array<size_t, 16> declaredSizes{
        262144, 524288, 1048576, 0, 0, 0, 0, 0, 0, 0, 8192, 16384, 32768, 49152, 65536, 131072};
    for (const size_t offset : {0x7ff0, 0x3ff0, 0x1ff0}) {
        if (offset + 16 > size || !std::equal(signature.begin(), signature.end(), bytes.begin() + offset)) continue;
        const uint8_t region = bytes[offset + 15] >> 4, code = bytes[offset + 15] & 15;
        const auto model = region == 3 || region == 4 ? RomModel::Sms
            : region >= 5 && region <= 7 ? RomModel::GameGear : RomModel::Unknown;
        metadata.header = SegaHeader{offset, static_cast<uint16_t>(bytes[offset + 10] | (bytes[offset + 11] << 8)),
            region, code, model, declaredSizes[code] != 0 ? std::optional<size_t>{declaredSizes[code]} : std::nullopt};
        break;
    }
    if(size>=32768 && std::has_single_bit(size)) {
        bool candidate=bytes[0x7fe0]==size/16384;
        constexpr std::array<unsigned,5> maximum{31,12,99,23,59};
        for(size_t i=0;i<maximum.size();++i) {
            const uint8_t raw=bytes[0x7fe1+i];
            const unsigned value=(raw>>4)*10+(raw&15);
            if((raw>>4)>9 || (raw&15)>9 || value>maximum[i] || (i<2 && value==0)) candidate=false;
        }
        if(candidate && std::all_of(bytes.begin()+0x7fea,bytes.begin()+0x7ff0,[](uint8_t v){return v==0;})) {
            auto word=[&](size_t at){return static_cast<uint16_t>(bytes[at]|(bytes[at+1]<<8));};
            uint16_t sum=0;
            for(size_t at=0;at<size;at+=2) {
                if(at>=0x7ff0 && at<0x8000) continue;
                sum=static_cast<uint16_t>(sum+word(at));
            }
            const auto stored=word(0x7fe6);
            const bool match=static_cast<uint16_t>(stored+word(0x7fe8))==0 && sum==stored;
            metadata.codemasters=CodemastersHeader{bytes[0x7fe0],stored,sum,
                match?ChecksumStatus::Match:ChecksumStatus::Mismatch};
        }
    }
    if(options.mapper==MapperKind::Auto) {
        if(metadata.codemasters && metadata.codemasters->status==ChecksumStatus::Match) {
            options.mapper=MapperKind::Codemasters;
            metadata.mapperSelection=MapperSelection::CodemastersHeader;
            if(options.sramBytes!=0 && options.sramBytes!=8192) return RomLoadError::IncompatibleOptions;
        } else if(size==49152) {
            options.mapper=MapperKind::RomOnly;
            metadata.mapperSelection=MapperSelection::Linear48K;
            if(options.sramBytes!=0) return RomLoadError::IncompatibleOptions;
        } else {
            options.mapper=MapperKind::Sega;
            metadata.mapperSelection=MapperSelection::DefaultSega;
        }
    }
    return RomImage(std::move(bytes), options, metadata);
}

} // namespace smsgg
