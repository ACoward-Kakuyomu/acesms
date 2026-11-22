#include "win32/DbSramStore.h"

#include <algorithm>
#include <cassert>

namespace smsgg::win32 {
DbSramStore::DbSramStore(RomDatabase& database,const RomRecord& rom)
    :database_(database),romId_(rom.id),mapper_(rom.settings.mapper),baseline_(rom.settings.mapper==MapperKind::Sega?rom.settings.ramBytes:0) {}
std::variant<std::vector<uint8_t>,DatabaseError> DbSramStore::load() {
    if(!enabled()) return std::vector<uint8_t>{};
    auto result=database_.readSram(romId_,baseline_.size());
    if(const auto* error=std::get_if<DatabaseError>(&result)) return *error;
    const auto& bytes=std::get<std::vector<uint8_t>>(result);
    std::copy(bytes.begin(),bytes.end(),baseline_.begin());
    return result;
}
std::optional<DatabaseError> DbSramStore::save(std::span<const uint8_t> current) {
    if(!enabled()) return std::nullopt;
    assert(current.size()==baseline_.size());
    if(std::equal(current.begin(),current.end(),baseline_.begin(),baseline_.end())) return std::nullopt;
    if(auto error=database_.writeSram(romId_,current)) return error;
    std::copy(current.begin(),current.end(),baseline_.begin());
    return std::nullopt;
}
std::optional<DatabaseError> DbSramStore::resize(std::vector<uint8_t>& current,uint32_t newBytes,const StateResizeContext* context) {
    if(mapper_!=MapperKind::Sega || (newBytes!=0 && newBytes!=8192 && newBytes!=16384 && newBytes!=32768))
        return DatabaseError{DatabaseErrorKind::InvalidSettings,0,"SRAM resize requires Sega and 0/8/16/32 KiB"};
    assert(current.size()==baseline_.size());
    if(newBytes==baseline_.size()) return std::nullopt;
    auto next=current;next.resize(newBytes,0);
    auto nextBaseline=next;
    if(auto error=database_.resizeSram(romId_,baseline_.size(),next,context)) return error;
    current.swap(next);baseline_.swap(nextBaseline);
    return std::nullopt;
}
std::optional<DatabaseError> DbSramStore::clear(std::span<uint8_t> current) {
    if(!enabled()) return std::nullopt;
    assert(current.size()==baseline_.size());
    std::vector<uint8_t> zero(baseline_.size());
    if(auto error=save(zero)) return error;
    std::ranges::fill(current,0);
    return std::nullopt;
}
std::variant<RomUpdateCounts,DatabaseError> DbSramStore::updateRom(RomRecord& record,std::vector<uint8_t>& current,
    const RomSettings& settings,std::wstring_view name,const std::filesystem::path& path,const RomUpdateContext* context) {
    auto result=prepareUpdate(record,current,settings,name,path,context);
    if(auto* error=std::get_if<DatabaseError>(&result)) return *error;
    return commitUpdate(std::get<PreparedRomUpdate>(result),record,current);
}
std::variant<PreparedRomUpdate,DatabaseError> DbSramStore::prepareUpdate(const RomRecord& record,std::span<const uint8_t> current,
    const RomSettings& settings,std::wstring_view name,const std::filesystem::path& path,const RomUpdateContext* context) {
    assert(record.id==romId_ && (!context || current.size()==record.settings.ramBytes));
    return database_.prepareUpdate(record,current,settings,name,path,context);
}
std::variant<RomUpdateCounts,DatabaseError> DbSramStore::commitUpdate(PreparedRomUpdate& prepared,RomRecord& record,std::vector<uint8_t>& current) {
    auto result=database_.commitUpdate(prepared,record,current,baseline_);
    if(std::holds_alternative<RomUpdateCounts>(result)) mapper_=record.settings.mapper;
    return result;
}
}
