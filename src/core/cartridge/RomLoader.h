#pragma once

#include "core/cartridge/RomImage.h"

#include <variant>
#include <filesystem>

namespace smsgg {

enum class RomLoadError { EmptyInput, UnsupportedRomSize, IncompatibleOptions, InvalidCopierHeader,
    OpenFailed, SizeFailed, ReadFailed };

class RomLoader final {
public:
    static std::variant<RomImage, RomLoadError> fromBytes(std::vector<uint8_t> bytes, LoadOptions options);
    static std::variant<RomImage, RomLoadError> load(const std::filesystem::path& path, LoadOptions options);
private:
    static std::variant<RomImage, RomLoadError> finish(std::vector<uint8_t> bytes, LoadOptions options);
};

} // namespace smsgg
