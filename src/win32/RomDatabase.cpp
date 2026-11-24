#include "win32/RomDatabase.h"
#include "core/cartridge/RomLoader.h"
#include <sqlite3.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <bit>
#include <cstring>
#include <cwchar>
#include <limits>
#include <utility>

namespace smsgg::win32 {
namespace {
DatabaseError sqlError(std::string_view path,int code,const char* reason) {
    return {DatabaseErrorKind::Sqlite,code,path.empty()?std::string(reason):"Database "+std::string(path)+": "+reason};
}
DatabaseError sqlError(sqlite3* db,int code) {
    const auto* path=sqlite3_db_filename(db,"main");
    return sqlError(path?path:"",code,sqlite3_errmsg(db));
}
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
std::variant<Statement,DatabaseError> prepare(sqlite3* db,const char* sql) {
    sqlite3_stmt* statement=nullptr;
    const auto code=sqlite3_prepare_v2(db,sql,-1,&statement,nullptr);
    Statement owner(statement,sqlite3_finalize);
    if(code!=SQLITE_OK) return sqlError(db,code);
    return owner;
}
std::optional<DatabaseError> writeShortcuts(sqlite3* db,const ShortcutBindings& shortcuts) {
    auto prepared=prepare(db,"INSERT OR REPLACE INTO app_shortcuts VALUES(1,?1,?2,?3,?4,?5,?6)");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* row=std::get<Statement>(prepared).get();
    for(size_t i=0;i<shortcuts.size();++i) {
        const auto code=sqlite3_bind_int(row,static_cast<int>(i+1),static_cast<int>(shortcuts[i]));
        if(code!=SQLITE_OK) return sqlError(db,code);
    }
    const auto code=sqlite3_step(row);
    if(code!=SQLITE_DONE) return sqlError(db,code);
    return {};
}
std::variant<int,DatabaseError> integer(sqlite3* db,const char* sql) {
    sqlite3_stmt* statement=nullptr;
    const auto prepared=sqlite3_prepare_v2(db,sql,-1,&statement,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owner(statement,sqlite3_finalize);
    if(prepared!=SQLITE_OK) return sqlError(db,prepared);
    const auto stepped=sqlite3_step(statement);
    if(stepped!=SQLITE_ROW) return sqlError(db,stepped);
    return sqlite3_column_int(statement,0);
}
bool textValid(std::wstring_view text) {
    return text.find(L'\0')==text.npos && text.size()<=static_cast<size_t>((std::numeric_limits<int>::max)())/sizeof(wchar_t);
}
bool settingsValid(const RomSettings& s,uint32_t bytes) {
    if(s.machine!=Machine::Sms && s.machine!=Machine::GameGear && s.machine!=Machine::Sg1000) return false;
    if(s.video!=VideoStandard::Ntsc && s.video!=VideoStandard::Pal) return false;
    if(s.ramBytes!=0 && s.ramBytes!=8192 && s.ramBytes!=16384 && s.ramBytes!=32768) return false;
    if(s.interpolation!=FmInterpolation::Sinc && s.interpolation!=FmInterpolation::ZohLowPass && s.interpolation!=FmInterpolation::Linear) return false;
    if(s.machine==Machine::GameGear && (s.video!=VideoStandard::Ntsc || s.fmEnabled)) return false;
    if(s.machine==Machine::Sg1000 && (s.video!=VideoStandard::Ntsc || s.fmEnabled || s.mapper!=MapperKind::RomOnly)) return false;
    if(s.fmEnabled && s.video!=VideoStandard::Ntsc) return false;
    if(!textValid(s.fmPlugin.native())) return false;
    if(!s.fmPlugin.empty() && (s.fmPlugin.has_parent_path() || _wcsicmp(s.fmPlugin.extension().c_str(),L".dll"))) return false;
    if(s.mapper==MapperKind::RomOnly) return !s.ramBytes && supportsLinearRomSize(bytes,s.machine);
    if(s.mapper==MapperKind::Sega) return supportsSegaRomSize(bytes,s.machine);
    if(s.mapper==MapperKind::Codemasters) return (s.ramBytes==0 || s.ramBytes==8192) && bytes>=32768 && bytes<=1048576 && std::has_single_bit(bytes);
    return false;
}
int bindText(sqlite3_stmt* statement,int index,std::wstring_view text) {
    return sqlite3_bind_text16(statement,index,text.empty()?L"":text.data(),static_cast<int>(text.size()*sizeof(wchar_t)),SQLITE_STATIC);
}
DatabaseError invalidRow() {return {DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid ROM database row"};}
DatabaseError missingState() {return {DatabaseErrorKind::InvalidData,SQLITE_OK,"Missing state for ROM"};}
std::variant<StateSummary,DatabaseError> readStateSummary(sqlite3* db,sqlite3_stmt* row,int64_t id) {
    if(sqlite3_column_type(row,0)!=SQLITE_TEXT || sqlite3_column_type(row,1)!=SQLITE_INTEGER ||
        sqlite3_column_type(row,2)!=SQLITE_INTEGER) return invalidRow();
    StateSummary summary{id,{},sqlite3_column_int64(row,1),sqlite3_column_int64(row,2)};
    // Wall-clock corrections may make updatedAt earlier than createdAt.
    if(summary.createdAt<0 || summary.updatedAt<0)
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid state timestamps"};
    const auto* name=static_cast<const wchar_t*>(sqlite3_column_text16(row,0));
    if(!name) return sqlError(db,SQLITE_NOMEM);
    summary.name.assign(name,sqlite3_column_bytes16(row,0)/sizeof(wchar_t));
    if(!textValid(summary.name))
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid state name"};
    return summary;
}
std::variant<RomRecord,DatabaseError> readRecord(sqlite3* db,sqlite3_stmt* s) {
    if(sqlite3_column_type(s,0)!=SQLITE_INTEGER || sqlite3_column_int64(s,0)<=0) return invalidRow();
    RomRecord out;out.id=sqlite3_column_int64(s,0);
    if(sqlite3_column_type(s,1)!=SQLITE_BLOB || sqlite3_column_bytes(s,1)!=32 ||
       sqlite3_column_type(s,2)!=SQLITE_BLOB || sqlite3_column_bytes(s,2)!=20) return invalidRow();
    const auto* sha256=sqlite3_column_blob(s,1);
    const auto* sha1=sqlite3_column_blob(s,2);
    if(!sha256 || !sha1) return sqlError(db,SQLITE_NOMEM);
    std::memcpy(out.identity.sha256.data(),sha256,32);
    std::memcpy(out.identity.sha1.data(),sha1,20);
    std::array<uint32_t,8> values{};
    constexpr int columns[]{3,4,7,8,9,10,11,13};
    for(size_t i=0;i<values.size();++i) {
        const auto n=sqlite3_column_int64(s,columns[i]);
        if(sqlite3_column_type(s,columns[i])!=SQLITE_INTEGER || n<0 || n>(std::numeric_limits<uint32_t>::max)()) return invalidRow();
        values[i]=static_cast<uint32_t>(n);
    }
    if(values[2]>static_cast<uint32_t>(Machine::Sg1000) || values[3]>1 || values[4]>2 || values[6]>1 || values[7]>2) return invalidRow();
    std::array<std::wstring,3> texts;
    constexpr int textColumns[]{5,6,12};
    for(size_t i=0;i<texts.size();++i) {
        const auto column=textColumns[i];
        if(sqlite3_column_type(s,column)!=SQLITE_TEXT) return invalidRow();
        const auto* text=static_cast<const wchar_t*>(sqlite3_column_text16(s,column));
        if(!text) return sqlError(db,SQLITE_NOMEM);
        texts[i].assign(text,sqlite3_column_bytes16(s,column)/sizeof(wchar_t));
        if(!textValid(texts[i])) return invalidRow();
    }
    out.identity.crc32=values[0];out.identity.bytes=values[1];
    out.settings={static_cast<Machine>(values[2]),static_cast<VideoStandard>(values[3]),static_cast<MapperKind>(values[4]),values[5],values[6]!=0,
        std::filesystem::path(texts[2]),static_cast<FmInterpolation>(values[7])};
    if(!settingsValid(out.settings,out.identity.bytes)) return invalidRow();
    out.name=std::move(texts[0]);out.lastPath=std::move(texts[1]);return out;
}
struct Rollback {
    sqlite3* db;
    ~Rollback() {if(db && !sqlite3_get_autocommit(db)) sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);}
};
std::variant<std::vector<uint8_t>,DatabaseError> encodeConvertedRam(DecodedState saved,
    const RomImage& target,const FmBackendPtr& backend,uint32_t newBytes) {
    saved.state->cartridge.ram.resize(newBytes,0);saved.profile.ramBytes=newBytes;
    auto converted=encodeState(saved.profile,*saved.state);
    auto checked=decodeState(converted,saved.profile);
    if(auto* error=std::get_if<StateCodecError>(&checked))
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Converted state validation failed: "+std::to_string(static_cast<int>(*error))};
    auto candidate=std::make_unique<EmulatorSystem>(target,saved.profile.video,saved.profile.sampleRate,saved.profile.machine,
        saved.profile.fm.has_value(),saved.profile.fm?saved.profile.fm->method:FmInterpolation::Sinc,backend);
    const auto result=candidate->restoreState(*std::get<std::unique_ptr<EmulatorSystemState>>(checked));
    if(result!=ACESMS_FM_OK) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Converted state backend restore failed: "+std::to_string(result)};
    return converted;
}
std::variant<std::vector<uint8_t>,DatabaseError> convertStateRam(std::span<const uint8_t> bytes,
    const StateResizeContext& context,const RomImage& target,uint32_t newBytes) {
    auto decoded=decodeStateAtSavedRate(bytes,context.profile);
    if(auto* error=std::get_if<StateCodecError>(&decoded))
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"State RAM conversion decode failed: "+std::to_string(static_cast<int>(*error))};
    return encodeConvertedRam(std::get<DecodedState>(std::move(decoded)),target,context.backend,newBytes);
}
bool matchesBackend(const FmStateProfile& p,const FmBackendPtr& backend) {
    const auto& d=backend->api().descriptor;
    return std::equal(p.backendId.begin(),p.backendId.end(),d.backend_id) && p.stateVersion==d.state_version &&
        std::equal(p.compatibilityId.begin(),p.compatibilityId.end(),d.compatibility_id) && p.clock==d.master_clock_hz && p.divider==d.native_divider;
}
struct StateEdit {int64_t id;bool remove;std::vector<uint8_t> bytes;};
struct StateChanges {std::vector<StateEdit> edits;RomUpdateCounts counts;};
std::variant<StateChanges,DatabaseError> prepareStateChanges(sqlite3* db,const RomRecord& before,
    const RomUpdateContext& context,const RomImage& target,const StateProfile& desired) {
    StateChanges result;
    auto states=prepare(db,"SELECT state_id,data FROM states WHERE rom_id=?1 ORDER BY state_id");
    if(auto* error=std::get_if<DatabaseError>(&states)) return *error;
    auto* row=std::get<Statement>(states).get();auto code=sqlite3_bind_int64(row,1,before.id);if(code!=SQLITE_OK) return sqlError(db,code);
    const auto minimumBytes=stateFileBytes({.machine=Machine::GameGear,.mapper=MapperKind::RomOnly});
    while((code=sqlite3_step(row))==SQLITE_ROW) {
        if(sqlite3_column_type(row,1)!=SQLITE_BLOB) return invalidRow();
        const auto size=static_cast<size_t>(sqlite3_column_bytes(row,1));
        if(size<minimumBytes) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid saved state BLOB length"};
        const auto* data=static_cast<const uint8_t*>(sqlite3_column_blob(row,1));
        if(!data) return sqlError(db,SQLITE_NOMEM);
        const auto bytes=std::span(data,size);auto inspected=inspectStateProfile(bytes);
        if(auto* error=std::get_if<StateCodecError>(&inspected)) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid saved state profile: "+std::to_string(static_cast<int>(*error))};
        const auto p=std::get<StateProfile>(inspected);
        if(p.romBytes!=before.identity.bytes || p.romCrc32!=before.identity.crc32) return invalidRow();
        FmBackendPtr backend;
        if(p.fm) {
            for(const auto& candidate:context.backends) if(candidate && matchesBackend(*p.fm,candidate)) {backend=candidate;break;}
            if(!backend) return DatabaseError{DatabaseErrorKind::InvalidSettings,SQLITE_OK,"Saved state backend is unavailable"};
        }
        auto decoded=decodeState(bytes,p);if(auto* error=std::get_if<StateCodecError>(&decoded))
            return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid saved state: "+std::to_string(static_cast<int>(*error))};
        const bool compatible=p.machine==desired.machine && p.video==desired.video && p.mapper==desired.mapper && p.fm==desired.fm;
        if(compatible && p.ramBytes!=desired.ramBytes) {
            auto changed=encodeConvertedRam({p,std::get<std::unique_ptr<EmulatorSystemState>>(std::move(decoded))},target,context.targetBackend,desired.ramBytes);
            if(auto* error=std::get_if<DatabaseError>(&changed)) return *error;
            result.edits.push_back({sqlite3_column_int64(row,0),false,std::get<std::vector<uint8_t>>(std::move(changed))});++result.counts.converted;
        } else {
            const auto payload=context.rom.bytes();
            auto source=std::get<RomImage>(RomLoader::fromBytes({payload.begin(),payload.end()},
                {p.mapper,p.ramBytes,CopierHeader::None,p.machine}));
            auto candidate=std::make_unique<EmulatorSystem>(source,p.video,p.sampleRate,p.machine,p.fm.has_value(),p.fm?p.fm->method:FmInterpolation::Sinc,compatible?context.targetBackend:backend);
            if(candidate->restoreState(*std::get<std::unique_ptr<EmulatorSystemState>>(decoded))!=ACESMS_FM_OK)
                return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Saved state backend validation failed"};
            if(compatible) ++result.counts.kept;
            else {result.edits.push_back({sqlite3_column_int64(row,0),true,{}});++result.counts.deleted;}
        }
    }
    if(code!=SQLITE_DONE) return sqlError(db,code);
    return result;
}
}
struct PreparedRomUpdate::Data {
    RomDatabase* database{};
    RomRecord record;
    std::vector<uint8_t> ram,baseline;
    RomUpdateCounts counts;
    bool changed{},oldPersistent{},newPersistent{};
    std::vector<StateEdit> edits;
    std::unique_ptr<EmulatorSystem> system;
    ~Data() {if(database && !sqlite3_get_autocommit(database->nativeHandle())) sqlite3_exec(database->nativeHandle(),"ROLLBACK",nullptr,nullptr,nullptr);}
};
PreparedRomUpdate::PreparedRomUpdate(std::unique_ptr<Data> data):data_(std::move(data)) {}
PreparedRomUpdate::~PreparedRomUpdate()=default;
PreparedRomUpdate::PreparedRomUpdate(PreparedRomUpdate&&) noexcept=default;
const RomRecord& PreparedRomUpdate::record() const noexcept {return data_->record;}
bool PreparedRomUpdate::changesSystem() const noexcept {return data_->changed;}
std::variant<std::vector<StateImpact>,DatabaseError> PreparedRomUpdate::stateImpact() const {
    auto listed=data_->database->listStates(data_->record.id);
    if(const auto* error=std::get_if<DatabaseError>(&listed)) return *error;
    std::vector<StateImpact> result;
    auto edit=data_->edits.begin();
    for(auto& state:std::get<std::vector<StateSummary>>(listed)) {
        auto disposition=StateDisposition::Keep;
        if(edit!=data_->edits.end() && edit->id==state.id) {
            disposition=edit->remove?StateDisposition::Delete:StateDisposition::Convert;
            ++edit;
        }
        result.push_back({std::move(state),disposition});
    }
    return result;
}
std::unique_ptr<EmulatorSystem> PreparedRomUpdate::takeSystem() noexcept {return std::move(data_->system);}
RomDatabase::~RomDatabase() {sqlite3_close_v2(db_);}
RomDatabase::RomDatabase(RomDatabase&& other) noexcept:db_(std::exchange(other.db_,nullptr)) {}
std::optional<DatabaseError> RomDatabase::execute(const char* sql) {
    const auto code=sqlite3_exec(db_,sql,nullptr,nullptr,nullptr);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    return std::nullopt;
}
std::variant<RomDatabase,DatabaseError> RomDatabase::open(const std::filesystem::path& path) {
    sqlite3* handle=nullptr;
    const auto code=sqlite3_open16(path.c_str(),&handle);
    RomDatabase db(handle);
    if(code!=SQLITE_OK || sqlite3_db_readonly(handle,"main")) {
        const auto utf8=path.u8string();
        const std::string filename(utf8.begin(),utf8.end());
        if(code!=SQLITE_OK) return sqlError(filename,code,sqlite3_errmsg(handle));
        return sqlError(filename,SQLITE_READONLY,"Database requires write access");
    }
    if(auto error=db.execute("PRAGMA foreign_keys=ON;BEGIN IMMEDIATE")) return *error;
    auto version=integer(handle,"PRAGMA user_version");
    if(auto* error=std::get_if<DatabaseError>(&version)) return *error;
    const auto versionNumber=std::get<int>(version);
    if(versionNumber==0) {
        auto objects=integer(handle,"SELECT count(*) FROM sqlite_schema WHERE name NOT GLOB 'sqlite_*'");
        if(auto* error=std::get_if<DatabaseError>(&objects)) return *error;
        if(std::get<int>(objects)!=0) return DatabaseError{DatabaseErrorKind::NotEmpty,SQLITE_OK,"Unversioned database is not empty"};
        if(auto error=db.execute(
            "CREATE TABLE roms(id INTEGER PRIMARY KEY,sha256 BLOB NOT NULL CHECK(length(sha256)=32),sha1 BLOB NOT NULL CHECK(length(sha1)=20),"
            "crc32 INTEGER NOT NULL,rom_bytes INTEGER NOT NULL,name TEXT NOT NULL,last_path TEXT NOT NULL,UNIQUE(sha256,rom_bytes)) STRICT;"
            "CREATE TABLE rom_settings(rom_id INTEGER PRIMARY KEY REFERENCES roms(id),machine INTEGER NOT NULL,video INTEGER NOT NULL,mapper INTEGER NOT NULL,"
            "ram_bytes INTEGER NOT NULL,fm_enabled INTEGER NOT NULL,fm_plugin TEXT NOT NULL,interpolation INTEGER NOT NULL) STRICT;"
            "CREATE TABLE sram(rom_id INTEGER PRIMARY KEY REFERENCES roms(id),data BLOB NOT NULL) STRICT;"
            "CREATE TABLE states(state_id INTEGER PRIMARY KEY,rom_id INTEGER NOT NULL REFERENCES roms(id),name TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL,data BLOB NOT NULL,slot INTEGER CHECK(slot BETWEEN 1 AND 10)) STRICT;"
            "CREATE UNIQUE INDEX state_slots ON states(rom_id,slot);"
            "CREATE TABLE app_settings(id INTEGER PRIMARY KEY CHECK(id=1),aspect INTEGER NOT NULL DEFAULT 0 CHECK(aspect IN(0,1)),"
            "machine INTEGER NOT NULL DEFAULT -1 CHECK(machine BETWEEN -1 AND 2),video INTEGER NOT NULL DEFAULT 0 CHECK(video IN(0,1)),"
            "mapper INTEGER NOT NULL DEFAULT 3 CHECK(mapper BETWEEN 0 AND 3),"
            "ram INTEGER NOT NULL DEFAULT 32768 CHECK(ram IN(0,8192,16384,32768)),"
            "copier INTEGER NOT NULL DEFAULT 0 CHECK(copier IN(0,1)),fm INTEGER NOT NULL DEFAULT 1 CHECK(fm IN(0,1)),"
            "plugin TEXT NOT NULL DEFAULT '',interpolation INTEGER NOT NULL DEFAULT 0 CHECK(interpolation BETWEEN 0 AND 2)) STRICT;"
            "INSERT INTO app_settings(id) VALUES(1);"
            "CREATE TABLE app_controllers(player INTEGER PRIMARY KEY CHECK(player IN(1,2)),"
            "source INTEGER NOT NULL CHECK(source BETWEEN 0 AND 2),slot INTEGER NOT NULL CHECK(slot BETWEEN 0 AND 3),"
            "k1 INTEGER NOT NULL,k2 INTEGER NOT NULL,k3 INTEGER NOT NULL,k4 INTEGER NOT NULL,"
            "k5 INTEGER NOT NULL,k6 INTEGER NOT NULL,k7 INTEGER NOT NULL,k8 INTEGER NOT NULL,"
            "b1 INTEGER NOT NULL,b2 INTEGER NOT NULL,b3 INTEGER NOT NULL,b4 INTEGER NOT NULL) STRICT;"
            "CREATE TABLE cheats(rom_id INTEGER NOT NULL REFERENCES roms(id),machine INTEGER NOT NULL,"
            "position INTEGER NOT NULL,name TEXT NOT NULL,address INTEGER NOT NULL,width INTEGER NOT NULL,"
            "value INTEGER NOT NULL,enabled INTEGER NOT NULL,always INTEGER NOT NULL,PRIMARY KEY(rom_id,machine,position)) STRICT;"
            )) return *error;
    } else {
        const auto reject=[&] {
            const auto* filename=sqlite3_db_filename(handle,"main");
            const auto reason="Unsupported database schema version "+std::to_string(versionNumber)+
                " (expected a supported version 1 or 2 schema)";
            return DatabaseError{DatabaseErrorKind::UnsupportedVersion,SQLITE_OK,
                filename && *filename?std::string(filename)+": "+reason:reason};
        };
        if(versionNumber!=1 && versionNumber!=2) return reject();
        auto required=integer(handle,"SELECT count(*) FROM sqlite_schema WHERE type='table' AND "
            "name IN('roms','rom_settings','sram','states','app_settings','app_controllers','cheats')");
        if(const auto* error=std::get_if<DatabaseError>(&required)) return *error;
        if(std::get<int>(required)!=7) return reject();
        if(versionNumber==2) {
            auto shortcuts=integer(handle,"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name='app_shortcuts'");
            if(const auto* error=std::get_if<DatabaseError>(&shortcuts)) return *error;
            if(std::get<int>(shortcuts)!=1) return reject();
        }
    }
    if(versionNumber<2) {
        auto bindings=defaultShortcuts;
        if(versionNumber==1) {
            auto prepared=prepare(handle,"SELECT b1,b2,b3,b4 FROM app_controllers WHERE player=1");
            if(const auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
            auto* row=std::get<Statement>(prepared).get();
            const auto code=sqlite3_step(row);
            if(code!=SQLITE_ROW && code!=SQLITE_DONE) return sqlError(handle,code);
            if(code==SQLITE_ROW) for(int column=0;column<4;++column)
                for(auto& binding:bindings) if(binding==sqlite3_column_int64(row,column)) binding=0;
        }
        if(auto error=db.execute("CREATE TABLE app_shortcuts(id INTEGER PRIMARY KEY CHECK(id=1),"
            "save INTEGER NOT NULL,load INTEGER NOT NULL,previous INTEGER NOT NULL,next INTEGER NOT NULL,"
            "fast_forward INTEGER NOT NULL,fullscreen INTEGER NOT NULL) STRICT;")) return *error;
        if(auto error=writeShortcuts(handle,bindings)) return *error;
        if(auto error=db.execute("PRAGMA user_version=2")) return *error;
    }
    if(auto error=db.execute("COMMIT")) return *error;
    return db;
}
bool validCheat(Machine machine,const Cheat& cheat) {
    return !cheat.name.empty() && cheat.name.size()<=64 && textValid(cheat.name) &&
        validWorkRamWrite(machine,cheat.address,cheat.width,cheat.value);
}

namespace {
std::optional<DatabaseError> validateCheats(Machine machine,std::span<const Cheat> cheats) {
    std::array<bool,workRamBytes(Machine::Sms)> frozen{};
    for(const auto& cheat:cheats) {
        if(!validCheat(machine,cheat)) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid cheat definition"};
        if(!cheat.enabled || !cheat.always) continue;
        const auto start=cheat.address-workRamBase;
        for(unsigned byte=0;byte<static_cast<unsigned>(cheat.width);++byte) {
            if(std::exchange(frozen[start+byte],true))
                return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Enabled always-apply cheats overlap"};
        }
    }
    return {};
}
}

std::variant<std::vector<Cheat>,DatabaseError> RomDatabase::readCheats(int64_t romId,Machine machine) {
    auto prepared=prepare(db_,"SELECT name,address,width,value,enabled,always FROM cheats WHERE rom_id=?1 AND machine=?2 ORDER BY position");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* row=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(row,1,romId);
    if(code==SQLITE_OK) code=sqlite3_bind_int(row,2,static_cast<int>(machine));
    if(code!=SQLITE_OK) return sqlError(db_,code);
    std::vector<Cheat> cheats;
    while((code=sqlite3_step(row))==SQLITE_ROW) {
        const auto* name=static_cast<const wchar_t*>(sqlite3_column_text16(row,0));
        const auto units=sqlite3_column_bytes16(row,0)/sizeof(wchar_t);
        const auto address=sqlite3_column_int64(row,1), width=sqlite3_column_int64(row,2), value=sqlite3_column_int64(row,3);
        const auto enabled=sqlite3_column_int64(row,4), always=sqlite3_column_int64(row,5);
        if(!name || units>64 || address<0 || address>0xffff || width<1 || width>4 || value<0 || value>UINT32_MAX ||
            enabled<0 || enabled>1 || always<0 || always>1) return invalidRow();
        cheats.push_back({std::wstring(name,units),static_cast<unsigned>(address),static_cast<SearchWidth>(width),
            static_cast<uint32_t>(value),enabled!=0,always!=0});
    }
    if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(auto error=validateCheats(machine,cheats)) return *error;
    return cheats;
}

std::optional<DatabaseError> RomDatabase::saveCheats(int64_t romId,Machine machine,std::span<const Cheat> cheats) {
    if(auto error=validateCheats(machine,cheats)) return error;
    if(auto error=execute("BEGIN IMMEDIATE")) return error;
    Rollback transaction{db_};
    auto erased=prepare(db_,"DELETE FROM cheats WHERE rom_id=?1 AND machine=?2");
    if(auto* error=std::get_if<DatabaseError>(&erased)) return *error;
    auto* row=std::get<Statement>(erased).get();
    auto code=sqlite3_bind_int64(row,1,romId);
    if(code==SQLITE_OK) code=sqlite3_bind_int(row,2,static_cast<int>(machine));
    if(code!=SQLITE_OK) return sqlError(db_,code);
    if((code=sqlite3_step(row))!=SQLITE_DONE) return sqlError(db_,code);
    auto inserted=prepare(db_,"INSERT INTO cheats(rom_id,machine,position,address,width,value,enabled,always,name) VALUES(?,?,?,?,?,?,?,?,?)");
    if(auto* error=std::get_if<DatabaseError>(&inserted)) return *error;
    row=std::get<Statement>(inserted).get();
    int64_t position=0;
    for(const auto& cheat:cheats) {
        const std::array<int64_t,8> fields{romId,static_cast<int>(machine),position++,cheat.address,
            static_cast<int>(cheat.width),cheat.value,cheat.enabled,cheat.always};
        for(int i=0;i<static_cast<int>(fields.size());++i) {
            if((code=sqlite3_bind_int64(row,i+1,fields[i]))!=SQLITE_OK) return sqlError(db_,code);
        }
        if((code=bindText(row,9,cheat.name))!=SQLITE_OK) return sqlError(db_,code);
        if((code=sqlite3_step(row))!=SQLITE_DONE) return sqlError(db_,code);
        if((code=sqlite3_reset(row))!=SQLITE_OK) return sqlError(db_,code);
    }
    return execute("COMMIT");
}

std::variant<AppSettings,DatabaseError> RomDatabase::readAppSettings() {
    if(auto error=execute("BEGIN")) return *error;
    Rollback rollback{db_};
    auto prepared=prepare(db_,"SELECT aspect,machine,video,mapper,ram,copier,fm,plugin,interpolation FROM app_settings WHERE id=1");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto& settings=std::get<Statement>(prepared);
    const auto code=sqlite3_step(settings.get());
    if(code!=SQLITE_ROW) return code==SQLITE_DONE?invalidRow():sqlError(db_,code);
    auto* row=settings.get();
    const auto value=[&](int col){return sqlite3_column_int64(row,col);};
    if(value(1)<-1 || value(1)>2 || value(2)<0 || value(2)>1 || value(3)<0 || value(3)>3 ||
        (value(4)!=0 && value(4)!=8192 && value(4)!=16384 && value(4)!=32768) ||
        value(5)<0 || value(5)>1 || value(8)<0 || value(8)>2) return invalidRow();
    AppSettings out;
    out.fourThree=value(0)!=0;
    if(value(1)>=0) out.rom.machine=static_cast<Machine>(value(1));
    out.rom.standard=static_cast<VideoStandard>(value(2));
    out.rom.options.mapper=static_cast<MapperKind>(value(3));
    out.rom.options.sramBytes=static_cast<size_t>(value(4));
    out.rom.options.copierHeader=static_cast<CopierHeader>(value(5));
    out.rom.fmEnabled=value(6)!=0;
    const auto* plugin=static_cast<const wchar_t*>(sqlite3_column_text16(row,7));
    if(!plugin) return sqlError(db_,SQLITE_NOMEM);
    const std::wstring_view name(plugin,sqlite3_column_bytes16(row,7)/sizeof(wchar_t));
    if(!textValid(name)) return invalidRow();
    out.rom.fmPlugin=name;
    out.rom.fmMethod=static_cast<FmInterpolation>(value(8));
    settings.reset();

    auto pads=prepare(db_,"SELECT player,source,slot,k1,k2,k3,k4,k5,k6,k7,k8,b1,b2,b3,b4 FROM app_controllers ORDER BY player");
    if(auto* error=std::get_if<DatabaseError>(&pads)) return *error;
    auto& statement=std::get<Statement>(pads);
    int result=sqlite3_step(statement.get());
    if(result==SQLITE_ROW) {
        auto& controllers=out.controllers.emplace();
        for(size_t i=0;i<controllers.size();++i) {
            if(result!=SQLITE_ROW) return result==SQLITE_DONE?invalidRow():sqlError(db_,result);
            if(sqlite3_column_int(statement.get(),0)!=static_cast<int>(i+1)) return invalidRow();
            auto& pad=controllers[i];
            pad.source=static_cast<PadSource>(sqlite3_column_int(statement.get(),1));
            pad.slot=sqlite3_column_int(statement.get(),2);
            int column=3;
            for(auto& key:pad.keys) {
                const auto stored=sqlite3_column_int64(statement.get(),column++);
                if(stored<0 || stored>255) return invalidRow();
                key=static_cast<unsigned>(stored);
            }
            for(auto& button:pad.buttons) {
                const auto stored=sqlite3_column_int64(statement.get(),column++);
                if(stored<0 || stored>65535) return invalidRow();
                button=static_cast<unsigned>(stored);
            }
            result=sqlite3_step(statement.get());
        }
    }
    if(result!=SQLITE_DONE) return result==SQLITE_ROW?invalidRow():sqlError(db_,result);
    auto shortcuts=prepare(db_,"SELECT save,load,previous,next,fast_forward,fullscreen FROM app_shortcuts WHERE id=1");
    if(const auto* error=std::get_if<DatabaseError>(&shortcuts)) return *error;
    auto* bindings=std::get<Statement>(shortcuts).get();
    const auto bindingResult=sqlite3_step(bindings);
    if(bindingResult!=SQLITE_ROW) return bindingResult==SQLITE_DONE?invalidRow():sqlError(db_,bindingResult);
    for(size_t i=0;i<out.shortcuts.size();++i) {
        const auto stored=sqlite3_column_int64(bindings,static_cast<int>(i));
        if(stored<0 || stored>rightTriggerButton) return invalidRow();
        out.shortcuts[i]=static_cast<unsigned>(stored);
    }
    if(validateShortcuts(out.shortcuts,out.controllers?(*out.controllers)[0]:PadConfiguration{})) return invalidRow();
    if(auto error=execute("COMMIT")) return *error;
    return out;
}

std::optional<DatabaseError> RomDatabase::saveAppSettings(const RomDefaults& rom,const ControllerConfiguration& controllers,
    const ShortcutBindings& shortcuts) {
    if(!textValid(rom.fmPlugin.native()) || validateShortcuts(shortcuts,controllers[0])) return invalidRow();
    if(auto error=execute("BEGIN IMMEDIATE")) return error;
    Rollback rollback{db_};
    auto prepared=prepare(db_,"UPDATE app_settings SET machine=?1,video=?2,mapper=?3,ram=?4,copier=?5,fm=?6,plugin=?7,interpolation=?8 WHERE id=1");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    const int values[]{rom.machine?static_cast<int>(*rom.machine):-1,static_cast<int>(rom.standard),
        static_cast<int>(rom.options.mapper),static_cast<int>(rom.options.sramBytes),
        static_cast<int>(rom.options.copierHeader),rom.fmEnabled};
    int index=1,code=SQLITE_OK;
    for(const auto value:values) if(code==SQLITE_OK) code=sqlite3_bind_int(s,index++,value);
    if(code==SQLITE_OK) code=bindText(s,7,rom.fmPlugin.native());
    if(code==SQLITE_OK) code=sqlite3_bind_int(s,8,static_cast<int>(rom.fmMethod));
    if(code!=SQLITE_OK) return sqlError(db_,code);
    if((code=sqlite3_step(s))!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return invalidRow();

    auto pads=prepare(db_,"INSERT OR REPLACE INTO app_controllers VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)");
    if(auto* error=std::get_if<DatabaseError>(&pads)) return *error;
    s=std::get<Statement>(pads).get();
    for(size_t i=0;i<controllers.size();++i) {
        const auto& pad=controllers[i];
        index=1;
        code=SQLITE_OK;
        const auto bind=[&](unsigned value) {if(code==SQLITE_OK) code=sqlite3_bind_int64(s,index++,value);};
        bind(static_cast<unsigned>(i+1));
        bind(static_cast<unsigned>(pad.source));
        bind(pad.slot);
        for(auto key:pad.keys) bind(key);
        for(auto button:pad.buttons) bind(button);
        if(code!=SQLITE_OK) return sqlError(db_,code);
        if((code=sqlite3_step(s))!=SQLITE_DONE) return sqlError(db_,code);
        if((code=sqlite3_reset(s))!=SQLITE_OK) return sqlError(db_,code);
    }
    if(auto error=writeShortcuts(db_,shortcuts)) return error;
    return execute("COMMIT");
}

std::optional<DatabaseError> RomDatabase::saveAspectRatio(bool fourThree) {
    if(auto error=execute(fourThree?"UPDATE app_settings SET aspect=1 WHERE id=1":"UPDATE app_settings SET aspect=0 WHERE id=1"))
        return error;
    if(sqlite3_changes(db_)!=1) return invalidRow();
    return std::nullopt;
}
std::variant<std::optional<RomRecord>,DatabaseError> RomDatabase::findRom(const RomIdentity& identity) {
    auto prepared=prepare(db_,"SELECT r.id,r.sha256,r.sha1,r.crc32,r.rom_bytes,r.name,r.last_path,s.machine,s.video,s.mapper,s.ram_bytes,s.fm_enabled,s.fm_plugin,s.interpolation "
        "FROM roms r LEFT JOIN rom_settings s ON s.rom_id=r.id WHERE r.sha256=?1 AND r.rom_bytes=?2");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_blob(s,1,identity.sha256.data(),static_cast<int>(identity.sha256.size()),SQLITE_STATIC);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,2,identity.bytes);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);
    if(code==SQLITE_DONE) return std::optional<RomRecord>{};
    if(code!=SQLITE_ROW) return sqlError(db_,code);
    auto record=readRecord(db_,s);
    if(auto* error=std::get_if<DatabaseError>(&record)) return *error;
    return std::optional<RomRecord>{std::get<RomRecord>(std::move(record))};
}
std::optional<DatabaseError> validateRomRegistration(const RomIdentity& identity,const RomSettings& settings,
    std::wstring_view name,const std::filesystem::path& path) {
    if(!settingsValid(settings,identity.bytes)) return DatabaseError{DatabaseErrorKind::InvalidSettings,SQLITE_OK,"Invalid ROM settings"};
    if(!textValid(name) || !textValid(path.native())) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid ROM metadata"};
    return std::nullopt;
}
std::variant<RomRecord,DatabaseError> RomDatabase::registerRom(const RomIdentity& identity,const RomSettings& settings,
    std::wstring_view name,const std::filesystem::path& path) {
    if(auto error=validateRomRegistration(identity,settings,name,path)) return *error;
    if(auto error=execute("BEGIN IMMEDIATE")) return *error;
    Rollback transaction{db_};
    auto existing=findRom(identity);
    if(auto* error=std::get_if<DatabaseError>(&existing)) return *error;
    if(auto& found=std::get<std::optional<RomRecord>>(existing);found) {
        if(auto error=execute("COMMIT")) return *error;
        return std::move(*found);
    }
    auto prepared=prepare(db_,"INSERT INTO roms(sha256,sha1,crc32,rom_bytes,name,last_path) VALUES(?1,?2,?3,?4,?5,?6)");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    std::optional<DatabaseError> bindError;
    auto bind=[&](int code){if(code!=SQLITE_OK && !bindError) bindError=sqlError(db_,code);};
    bind(sqlite3_bind_blob(s,1,identity.sha256.data(),static_cast<int>(identity.sha256.size()),SQLITE_STATIC));
    bind(sqlite3_bind_blob(s,2,identity.sha1.data(),static_cast<int>(identity.sha1.size()),SQLITE_STATIC));
    bind(sqlite3_bind_int64(s,3,identity.crc32));bind(sqlite3_bind_int64(s,4,identity.bytes));
    bind(bindText(s,5,name));bind(bindText(s,6,path.native()));
    if(bindError) return *bindError;
    auto code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);
    const auto id=sqlite3_last_insert_rowid(db_);
    auto parameters=prepare(db_,"INSERT INTO rom_settings(rom_id,machine,video,mapper,ram_bytes,fm_enabled,fm_plugin,interpolation) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    if(auto* error=std::get_if<DatabaseError>(&parameters)) return *error;
    s=std::get<Statement>(parameters).get();
    bind(sqlite3_bind_int64(s,1,id));bind(sqlite3_bind_int(s,2,static_cast<int>(settings.machine)));
    bind(sqlite3_bind_int(s,3,static_cast<int>(settings.video)));bind(sqlite3_bind_int(s,4,static_cast<int>(settings.mapper)));
    bind(sqlite3_bind_int64(s,5,settings.ramBytes));bind(sqlite3_bind_int(s,6,settings.fmEnabled));
    bind(bindText(s,7,settings.fmPlugin.native()));bind(sqlite3_bind_int(s,8,static_cast<int>(settings.interpolation)));
    if(bindError) return *bindError;
    code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(settings.mapper==MapperKind::Sega && settings.ramBytes) {
        auto memory=prepare(db_,"INSERT INTO sram(rom_id,data) VALUES(?1,?2)");
        if(auto* error=std::get_if<DatabaseError>(&memory)) return *error;
        auto* blob=std::get<Statement>(memory).get();
        bind(sqlite3_bind_int64(blob,1,id));bind(sqlite3_bind_zeroblob(blob,2,static_cast<int>(settings.ramBytes)));
        if(bindError) return *bindError;
        code=sqlite3_step(blob);if(code!=SQLITE_DONE) return sqlError(db_,code);
    }
    RomRecord saved{id,identity,settings,std::wstring(name),path};
    if(auto error=execute("COMMIT")) return *error;
    return saved;
}
std::variant<std::vector<uint8_t>,DatabaseError> RomDatabase::readSram(int64_t id,size_t expectedBytes) {
    auto prepared=prepare(db_,"SELECT data FROM sram WHERE rom_id=?1");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(s,1,id);if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);
    if(code==SQLITE_DONE) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Missing SRAM row"};
    if(code!=SQLITE_ROW) return sqlError(db_,code);
    if(sqlite3_column_type(s,0)!=SQLITE_BLOB || static_cast<size_t>(sqlite3_column_bytes(s,0))!=expectedBytes)
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid SRAM type or capacity"};
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(s,0));
    if(!bytes) return sqlError(db_,SQLITE_NOMEM);
    return std::vector<uint8_t>(bytes,bytes+expectedBytes);
}
std::optional<DatabaseError> RomDatabase::writeSram(int64_t id,std::span<const uint8_t> current) {
    if(auto error=execute("BEGIN IMMEDIATE")) return error;
    Rollback transaction{db_};
    auto prepared=prepare(db_,"UPDATE sram SET data=?1 WHERE rom_id=?2 AND typeof(data)='blob' AND length(data)=?3");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_blob(s,1,current.data(),static_cast<int>(current.size()),SQLITE_STATIC);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,2,id);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,3,current.size());
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Missing or invalid SRAM row"};
    return execute("COMMIT");
}
std::optional<DatabaseError> RomDatabase::resizeSram(int64_t romId,size_t oldBytes,std::span<const uint8_t> next,const StateResizeContext* context) {
    if(auto error=execute("BEGIN IMMEDIATE")) return error;
    Rollback transaction{db_};
    auto prepared=prepare(db_,"SELECT r.mapper,r.ram_bytes,s.data,EXISTS(SELECT 1 FROM states WHERE rom_id=?1) "
        "FROM rom_settings r LEFT JOIN sram s ON s.rom_id=r.rom_id WHERE r.rom_id=?1");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(s,1,romId);if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);
    if(code==SQLITE_DONE) return invalidRow();
    if(code!=SQLITE_ROW) return sqlError(db_,code);
    if(sqlite3_column_int64(s,0)!=static_cast<int64_t>(MapperKind::Sega) || sqlite3_column_int64(s,1)!=static_cast<int64_t>(oldBytes) ||
       (oldBytes?(sqlite3_column_type(s,2)!=SQLITE_BLOB || static_cast<size_t>(sqlite3_column_bytes(s,2))!=oldBytes):sqlite3_column_type(s,2)!=SQLITE_NULL))
        return invalidRow();
    const bool hasStates=sqlite3_column_int(s,3)!=0;
    std::get<Statement>(prepared).reset();
    std::vector<std::pair<int64_t,std::vector<uint8_t>>> converted;
    if(hasStates) {
        if(!context || context->profile.ramBytes!=oldBytes || (context->profile.fm && !context->backend))
            return DatabaseError{DatabaseErrorKind::InvalidSettings,SQLITE_OK,"State resize requires the current ROM/profile and resolved FM backend"};
        const auto& source=context->rom.bytes();
        auto target=std::get<RomImage>(RomLoader::fromBytes({source.begin(),source.end()},{MapperKind::Sega,next.size()}));
        auto states=prepare(db_,"SELECT state_id,data FROM states WHERE rom_id=?1 ORDER BY state_id");
        if(auto* error=std::get_if<DatabaseError>(&states)) return *error;
        auto* row=std::get<Statement>(states).get();
        code=sqlite3_bind_int64(row,1,romId);if(code!=SQLITE_OK) return sqlError(db_,code);
        const auto expectedSize=stateFileBytes(context->profile);
        while((code=sqlite3_step(row))==SQLITE_ROW) {
            if(sqlite3_column_type(row,1)!=SQLITE_BLOB) return invalidRow();
            const auto size=static_cast<size_t>(sqlite3_column_bytes(row,1));
            if(size!=expectedSize) return invalidRow();
            const auto* data=static_cast<const uint8_t*>(sqlite3_column_blob(row,1));if(!data) return sqlError(db_,SQLITE_NOMEM);
            auto bytes=convertStateRam(std::span(data,size),*context,target,static_cast<uint32_t>(next.size()));
            if(auto* error=std::get_if<DatabaseError>(&bytes)) return *error;
            converted.emplace_back(sqlite3_column_int64(row,0),std::get<std::vector<uint8_t>>(std::move(bytes)));
        }
        if(code!=SQLITE_DONE) return sqlError(db_,code);
    }
    auto settings=prepare(db_,"UPDATE rom_settings SET ram_bytes=?1 WHERE rom_id=?2");
    if(auto* error=std::get_if<DatabaseError>(&settings)) return *error;
    s=std::get<Statement>(settings).get();
    code=sqlite3_bind_int64(s,1,next.size());
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,2,romId);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return invalidRow();
    auto memory=prepare(db_,next.empty()?"DELETE FROM sram WHERE rom_id=?1":
        oldBytes?"UPDATE sram SET data=?2 WHERE rom_id=?1":"INSERT INTO sram(rom_id,data) VALUES(?1,?2)");
    if(auto* error=std::get_if<DatabaseError>(&memory)) return *error;
    s=std::get<Statement>(memory).get();
    code=sqlite3_bind_int64(s,1,romId);
    if(code==SQLITE_OK && !next.empty()) code=sqlite3_bind_blob(s,2,next.data(),static_cast<int>(next.size()),SQLITE_STATIC);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return invalidRow();
    for(const auto& [id,bytes]:converted) {
        auto state=prepare(db_,"UPDATE states SET data=?1 WHERE state_id=?2 AND rom_id=?3");
        if(auto* error=std::get_if<DatabaseError>(&state)) return *error;
        auto* row=std::get<Statement>(state).get();
        code=sqlite3_bind_blob64(row,1,bytes.data(),bytes.size(),SQLITE_STATIC);
        if(code==SQLITE_OK) code=sqlite3_bind_int64(row,2,id);
        if(code==SQLITE_OK) code=sqlite3_bind_int64(row,3,romId);
        if(code!=SQLITE_OK) return sqlError(db_,code);
        code=sqlite3_step(row);if(code!=SQLITE_DONE) return sqlError(db_,code);
        if(sqlite3_changes(db_)!=1) return invalidRow();
    }
    return execute("COMMIT");
}
std::variant<PreparedRomUpdate,DatabaseError> RomDatabase::prepareUpdate(const RomRecord& before,std::span<const uint8_t> current,
    const RomSettings& settings,std::wstring_view name,const std::filesystem::path& path,const RomUpdateContext* context) {
    if(!settingsValid(settings,before.identity.bytes)) return DatabaseError{DatabaseErrorKind::InvalidSettings,SQLITE_OK,"Invalid ROM settings"};
    if(!textValid(name) || !textValid(path.native())) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid ROM metadata"};
    if(!context && settings!=before.settings) return DatabaseError{DatabaseErrorKind::InvalidSettings,SQLITE_OK,"Settings update requires ROM resources"};
    if(context && (context->rom.bytes().size()!=before.identity.bytes || context->rom.metadata().crc32!=before.identity.crc32))
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"ROM update image does not match the database record"};
    if(auto error=execute("BEGIN IMMEDIATE")) return *error;
    Rollback transaction{db_};
    auto found=findRom(before.identity);
    if(auto* error=std::get_if<DatabaseError>(&found)) return *error;
    const auto& existing=std::get<std::optional<RomRecord>>(found);
    if(!existing || *existing!=before) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"ROM record changed; reload before updating"};
    auto data=std::make_unique<PreparedRomUpdate::Data>();auto& result=*data;
    result.record=before;result.record.settings=settings;result.record.name=name;result.record.lastPath=path;result.changed=context!=nullptr;
    const bool oldPersistent=before.settings.mapper==MapperKind::Sega && before.settings.ramBytes;
    const bool newPersistent=settings.mapper==MapperKind::Sega && settings.ramBytes;
    result.oldPersistent=oldPersistent;result.newPersistent=newPersistent;
    if(context) {
        if(!context->sampleRate || context->sampleRate>cpuClockHz(settings.video) || (settings.fmEnabled && !context->targetBackend))
            return DatabaseError{DatabaseErrorKind::InvalidSettings,SQLITE_OK,"Invalid restart rate or unresolved target backend"};
        if(oldPersistent) {auto old=readSram(before.id,before.settings.ramBytes);if(auto* error=std::get_if<DatabaseError>(&old)) return *error;}
        else {
            auto old=prepare(db_,"SELECT rom_id FROM sram WHERE rom_id=?1");if(auto* error=std::get_if<DatabaseError>(&old)) return *error;
            auto* row=std::get<Statement>(old).get();auto code=sqlite3_bind_int64(row,1,before.id);if(code!=SQLITE_OK) return sqlError(db_,code);
            code=sqlite3_step(row);if(code==SQLITE_ROW) return invalidRow();if(code!=SQLITE_DONE) return sqlError(db_,code);
        }
        result.ram.assign(current.begin(),current.end());
        if(settings.mapper!=before.settings.mapper) result.ram.clear();
        result.ram.resize(settings.ramBytes,0);if(newPersistent) result.baseline=result.ram;
        const auto payload=context->rom.bytes();
        auto target=std::get<RomImage>(RomLoader::fromBytes({payload.begin(),payload.end()},settings.loadOptions()));
        auto restart=std::make_unique<EmulatorSystem>(target,settings.video,context->sampleRate,settings.machine,settings.fmEnabled,settings.interpolation,context->targetBackend);
        auto initial=std::make_unique<EmulatorSystemState>();restart->captureState(*initial);initial->cartridge.ram=result.ram;
        if(restart->restoreState(*initial)!=ACESMS_FM_OK) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Restart candidate failed"};
        StateProfile desired{before.identity.bytes,before.identity.crc32,settings.machine,settings.video,settings.mapper,settings.ramBytes,context->sampleRate,{}};
        if(settings.fmEnabled) {
            const auto& d=context->targetBackend->api().descriptor;FmStateProfile f;
            std::copy_n(d.backend_id,16,f.backendId.begin());std::copy_n(d.compatibility_id,16,f.compatibilityId.begin());
            f.stateVersion=d.state_version;f.clock=d.master_clock_hz;f.divider=d.native_divider;f.backendBytes=static_cast<uint32_t>(initial->audio.fm->backend.size());
            f.method=settings.interpolation;f.resamplerVersion=1;desired.fm=f;
        }
        auto states=prepareStateChanges(db_,before,*context,target,desired);if(auto* error=std::get_if<DatabaseError>(&states)) return *error;
        auto& changes=std::get<StateChanges>(states);result.edits=std::move(changes.edits);result.counts=changes.counts;result.system=std::move(restart);
    } else {
        auto count=prepare(db_,"SELECT count(*) FROM states WHERE rom_id=?1");if(auto* error=std::get_if<DatabaseError>(&count)) return *error;
        auto* row=std::get<Statement>(count).get();auto code=sqlite3_bind_int64(row,1,before.id);if(code!=SQLITE_OK) return sqlError(db_,code);
        code=sqlite3_step(row);if(code!=SQLITE_ROW) return sqlError(db_,code);result.counts.kept=static_cast<size_t>(sqlite3_column_int64(row,0));
    }
    result.database=this;transaction.db=nullptr;
    return PreparedRomUpdate(std::move(data));
}
std::variant<RomUpdateCounts,DatabaseError> RomDatabase::commitUpdate(PreparedRomUpdate& prepared,RomRecord& record,
    std::vector<uint8_t>& current,std::vector<uint8_t>& baseline) {
    auto& result=*prepared.data_;assert(result.database==this);
    result.database=nullptr;Rollback transaction{db_};
    const auto& settings=result.record.settings;const auto& name=result.record.name;const auto& path=result.record.lastPath;
    const auto id=result.record.id;
    auto metadata=prepare(db_,"UPDATE roms SET name=?1,last_path=?2 WHERE id=?3");if(auto* error=std::get_if<DatabaseError>(&metadata)) return *error;
    auto* s=std::get<Statement>(metadata).get();auto code=bindText(s,1,name);if(code==SQLITE_OK) code=bindText(s,2,path.native());
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,3,id);if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);if(sqlite3_changes(db_)!=1) return invalidRow();
    if(result.changed) {
        auto parameters=prepare(db_,"UPDATE rom_settings SET machine=?1,video=?2,mapper=?3,ram_bytes=?4,fm_enabled=?5,fm_plugin=?6,interpolation=?7 WHERE rom_id=?8");
        if(auto* error=std::get_if<DatabaseError>(&parameters)) return *error;
        s=std::get<Statement>(parameters).get();code=sqlite3_bind_int(s,1,static_cast<int>(settings.machine));
        if(code==SQLITE_OK) code=sqlite3_bind_int(s,2,static_cast<int>(settings.video));if(code==SQLITE_OK) code=sqlite3_bind_int(s,3,static_cast<int>(settings.mapper));
        if(code==SQLITE_OK) code=sqlite3_bind_int64(s,4,settings.ramBytes);if(code==SQLITE_OK) code=sqlite3_bind_int(s,5,settings.fmEnabled);
        if(code==SQLITE_OK) code=bindText(s,6,settings.fmPlugin.native());if(code==SQLITE_OK) code=sqlite3_bind_int(s,7,static_cast<int>(settings.interpolation));
        if(code==SQLITE_OK) code=sqlite3_bind_int64(s,8,id);if(code!=SQLITE_OK) return sqlError(db_,code);
        code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);if(sqlite3_changes(db_)!=1) return invalidRow();
        if(result.oldPersistent || result.newPersistent) {
            auto memory=prepare(db_,!result.newPersistent?"DELETE FROM sram WHERE rom_id=?1":result.oldPersistent?"UPDATE sram SET data=?2 WHERE rom_id=?1":"INSERT INTO sram(rom_id,data) VALUES(?1,?2)");
            if(auto* error=std::get_if<DatabaseError>(&memory)) return *error;
            s=std::get<Statement>(memory).get();code=sqlite3_bind_int64(s,1,id);
            if(code==SQLITE_OK && result.newPersistent) code=sqlite3_bind_blob(s,2,result.ram.data(),static_cast<int>(result.ram.size()),SQLITE_STATIC);
            if(code!=SQLITE_OK) return sqlError(db_,code);code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);if(sqlite3_changes(db_)!=1) return invalidRow();
        }
        for(const auto& edit:result.edits) {
            auto state=prepare(db_,edit.remove?"DELETE FROM states WHERE state_id=?1 AND rom_id=?2":"UPDATE states SET data=?3 WHERE state_id=?1 AND rom_id=?2");
            if(auto* error=std::get_if<DatabaseError>(&state)) return *error;
            s=std::get<Statement>(state).get();code=sqlite3_bind_int64(s,1,edit.id);if(code==SQLITE_OK) code=sqlite3_bind_int64(s,2,id);
            if(code==SQLITE_OK && !edit.remove) code=sqlite3_bind_blob64(s,3,edit.bytes.data(),edit.bytes.size(),SQLITE_STATIC);
            if(code!=SQLITE_OK) return sqlError(db_,code);code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);if(sqlite3_changes(db_)!=1) return invalidRow();
        }
    }
    std::get<Statement>(metadata).reset();
    if(auto error=execute("COMMIT")) return *error;
    std::swap(record,result.record);
    if(result.changed) {current.swap(result.ram);baseline.swap(result.baseline);}
    return result.counts;
}
std::variant<int64_t,DatabaseError> RomDatabase::saveState(int64_t romId,std::wstring_view name,
    const StateProfile& profile,const EmulatorSystemState& snapshot,std::optional<int64_t> stateId) {
    if(!textValid(name)) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid state name"};
    const auto bytes=encodeState(profile,snapshot);
    if(auto error=execute("BEGIN IMMEDIATE")) return *error;
    Rollback transaction{db_};
    auto prepared=prepare(db_,stateId?
        "UPDATE states SET name=?1,data=?2,updated_at=unixepoch() WHERE rom_id=?3 AND state_id=?4":
        "INSERT INTO states(name,data,rom_id,created_at,updated_at) VALUES(?1,?2,?3,unixepoch(),unixepoch())");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    auto code=bindText(s,1,name);
    if(code==SQLITE_OK) code=sqlite3_bind_blob64(s,2,bytes.data(),bytes.size(),SQLITE_STATIC);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,3,romId);
    if(code==SQLITE_OK && stateId) code=sqlite3_bind_int64(s,4,*stateId);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return missingState();
    const auto id=stateId?*stateId:sqlite3_last_insert_rowid(db_);
    if(auto error=execute("COMMIT")) return *error;
    return id;
}
std::variant<SavedState,DatabaseError,StateCodecError> RomDatabase::readState(int64_t romId,int64_t stateId,const StateProfile& profile) {
    auto prepared=prepare(db_,"SELECT name,created_at,updated_at,data FROM states WHERE rom_id=?1 AND state_id=?2");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* s=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(s,1,romId);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(s,2,stateId);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(s);
    if(code==SQLITE_DONE) return missingState();
    if(code!=SQLITE_ROW) return sqlError(db_,code);
    const auto expectedSize=stateFileBytes(profile);
    const auto size=sqlite3_column_type(s,3)==SQLITE_BLOB?static_cast<size_t>(sqlite3_column_bytes(s,3)):0;
    if(size!=expectedSize)
        return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid state row or size"};
    auto metadata=readStateSummary(db_,s,stateId);
    if(auto* error=std::get_if<DatabaseError>(&metadata)) return *error;
    const auto* bytes=static_cast<const uint8_t*>(sqlite3_column_blob(s,3));
    if(!bytes) return sqlError(db_,SQLITE_NOMEM);
    auto decoded=decodeState(std::span(bytes,size),profile);
    if(auto* error=std::get_if<StateCodecError>(&decoded)) return *error;
    auto& summary=std::get<StateSummary>(metadata);
    return SavedState{summary.id,std::move(summary.name),summary.createdAt,summary.updatedAt,
        std::get<std::unique_ptr<EmulatorSystemState>>(std::move(decoded))};
}
std::variant<std::vector<StateSummary>,DatabaseError> RomDatabase::listStates(int64_t romId) {
    auto prepared=prepare(db_,"SELECT name,created_at,updated_at,state_id,slot FROM states WHERE rom_id=?1 ORDER BY slot IS NULL,slot,state_id");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* row=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(row,1,romId);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    std::vector<StateSummary> states;
    while((code=sqlite3_step(row))==SQLITE_ROW) {
        auto summary=readStateSummary(db_,row,sqlite3_column_int64(row,3));
        if(auto* error=std::get_if<DatabaseError>(&summary)) return *error;
        auto item=std::get<StateSummary>(std::move(summary));
        item.slot=static_cast<unsigned>(sqlite3_column_int(row,4));
        states.push_back(std::move(item));
    }
    if(code!=SQLITE_DONE) return sqlError(db_,code);
    return states;
}
std::variant<int64_t,DatabaseError> RomDatabase::saveSlot(int64_t romId,unsigned slot,
    const StateProfile& profile,const EmulatorSystemState& snapshot) {
    if(slot<1 || slot>10) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid slot"};
    const auto bytes=encodeState(profile,snapshot);
    if(auto error=execute("BEGIN IMMEDIATE")) return *error;
    Rollback transaction{db_};
    auto prepared=prepare(db_,"INSERT INTO states(rom_id,slot,name,created_at,updated_at,data)"
        " VALUES(?1,?2,'',unixepoch(),unixepoch(),?3)"
        " ON CONFLICT(rom_id,slot) DO UPDATE SET data=excluded.data,updated_at=excluded.updated_at RETURNING state_id");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* row=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(row,1,romId);
    if(code==SQLITE_OK) code=sqlite3_bind_int(row,2,slot);
    if(code==SQLITE_OK) code=sqlite3_bind_blob64(row,3,bytes.data(),bytes.size(),SQLITE_STATIC);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(row);if(code!=SQLITE_ROW) return sqlError(db_,code);
    const auto id=sqlite3_column_int64(row,0);
    code=sqlite3_step(row);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(auto error=execute("COMMIT")) return *error;
    return id;
}
std::optional<DatabaseError> RomDatabase::renameState(int64_t romId,int64_t stateId,std::wstring_view name) {
    if(!textValid(name)) return DatabaseError{DatabaseErrorKind::InvalidData,SQLITE_OK,"Invalid state name"};
    auto prepared=prepare(db_,"UPDATE states SET name=?1 WHERE rom_id=?2 AND state_id=?3");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* row=std::get<Statement>(prepared).get();
    auto code=bindText(row,1,name);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(row,2,romId);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(row,3,stateId);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(row);if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return missingState();
    return {};
}
std::optional<DatabaseError> RomDatabase::deleteState(int64_t romId,int64_t stateId) {
    if(auto error=execute("BEGIN IMMEDIATE")) return error;
    Rollback transaction{db_};
    auto prepared=prepare(db_,"DELETE FROM states WHERE rom_id=?1 AND state_id=?2");
    if(auto* error=std::get_if<DatabaseError>(&prepared)) return *error;
    auto* row=std::get<Statement>(prepared).get();
    auto code=sqlite3_bind_int64(row,1,romId);
    if(code==SQLITE_OK) code=sqlite3_bind_int64(row,2,stateId);
    if(code!=SQLITE_OK) return sqlError(db_,code);
    code=sqlite3_step(row);
    if(code!=SQLITE_DONE) return sqlError(db_,code);
    if(sqlite3_changes(db_)!=1) return missingState();
    return execute("COMMIT");
}
}
