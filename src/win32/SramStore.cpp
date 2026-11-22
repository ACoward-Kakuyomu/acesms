#include "win32/SramStore.h"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

namespace smsgg::win32 {
SramStore::SramStore(const std::filesystem::path& romPath,const RomImage& image) {
    if(image.options().mapper!=MapperKind::Sega || !image.options().sramBytes) return;
    baseline_.resize(image.options().sramBytes);
    std::wostringstream suffix;
    suffix<<L'.'<<std::hex<<std::setfill(L'0')<<std::setw(8)<<image.metadata().crc32<<L'-'<<std::dec<<image.bytes().size()<<L".sav";
    path_=romPath.parent_path()/(romPath.stem().wstring()+suffix.str());
}
std::variant<std::vector<uint8_t>,FileError> SramStore::load() {
    if(!enabled()) return std::vector<uint8_t>{};
    auto result=readFileExact(path_,baseline_.size());std::vector<uint8_t> bytes;
    if(const auto* error=std::get_if<FileError>(&result)) {
        if(error->operation!=FileOperation::Open || error->code!=ERROR_FILE_NOT_FOUND) return *error;
        bytes.resize(baseline_.size());
    } else bytes=std::get<std::vector<uint8_t>>(std::move(result));
    std::copy(bytes.begin(),bytes.end(),baseline_.begin());return bytes;
}
std::optional<FileError> SramStore::save(std::span<const uint8_t> current) {
    if(!enabled()) return std::nullopt;
    assert(current.size()==baseline_.size());
    if(std::equal(current.begin(),current.end(),baseline_.begin())) return std::nullopt;
    if(auto error=writeFileAtomically(path_,current)) return error;
    std::copy(current.begin(),current.end(),baseline_.begin());return std::nullopt;
}
}
