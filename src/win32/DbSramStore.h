#pragma once

#include "win32/RomDatabase.h"

namespace smsgg::win32 {
// Same owning thread. The database outlives the store and remains unmoved.
class DbSramStore final {
public:
    DbSramStore(RomDatabase& database,const RomRecord& rom);
    DbSramStore(const DbSramStore&)=delete;
    DbSramStore& operator=(const DbSramStore&)=delete;
    bool enabled() const noexcept {return !baseline_.empty();}
    std::variant<std::vector<uint8_t>,DatabaseError> load();
    // Call after load succeeds, with the same validated ROM/capacity. Disabled stores do no SQL.
    std::optional<DatabaseError> save(std::span<const uint8_t> current);
    // Latest owned RAM after a successful load/resize; both RAM and baseline change only on success.
    std::optional<DatabaseError> resize(std::vector<uint8_t>& current,uint32_t newBytes,const StateResizeContext* context=nullptr);
    std::optional<DatabaseError> clear(std::span<uint8_t> current);
    std::variant<RomUpdateCounts,DatabaseError> updateRom(RomRecord& record,std::vector<uint8_t>& current,
        const RomSettings& settings,std::wstring_view name,const std::filesystem::path& path,const RomUpdateContext* context=nullptr);
    std::variant<PreparedRomUpdate,DatabaseError> prepareUpdate(const RomRecord& record,std::span<const uint8_t> current,
        const RomSettings& settings,std::wstring_view name,const std::filesystem::path& path,const RomUpdateContext* context=nullptr);
    std::variant<RomUpdateCounts,DatabaseError> commitUpdate(PreparedRomUpdate& prepared,RomRecord& record,std::vector<uint8_t>& current);
private:
    RomDatabase& database_;
    int64_t romId_;
    MapperKind mapper_;
    std::vector<uint8_t> baseline_;
};
}
