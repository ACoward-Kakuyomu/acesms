#pragma once

#include "core/cartridge/RomImage.h"
#include "win32/FileStore.h"

namespace smsgg::win32 {
class SramStore final {
public:
    SramStore(const std::filesystem::path& romPath,const RomImage& image);
    bool enabled() const noexcept {return !baseline_.empty();}
    const std::filesystem::path& path() const noexcept {return path_;}
    std::variant<std::vector<uint8_t>,FileError> load();
    // Current RAM belongs to the same validated ROM/capacity. Disabled stores do no I/O.
    std::optional<FileError> save(std::span<const uint8_t> current);
private:
    std::filesystem::path path_;
    std::vector<uint8_t> baseline_;
};
}
