#pragma once

#include "core/Machine.h"
#include "core/cartridge/RomImage.h"
#include "core/audio/FmResampler.h"
#include "core/video/Vdp.h"
#include "core/state/StateCodec.h"
#include "win32/InputManager.h"
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <variant>
#include <vector>

struct sqlite3;
namespace smsgg::win32 {
enum class DatabaseErrorKind {Sqlite,UnsupportedVersion,NotEmpty,Hash,InvalidSettings,InvalidData};
struct DatabaseError {DatabaseErrorKind kind;int code;std::string message;};
struct RomDefaults {
    LoadOptions options{MapperKind::Auto,32768};
    VideoStandard standard{VideoStandard::Ntsc};
    std::optional<Machine> machine;
    bool fmEnabled{true};
    std::filesystem::path fmPlugin;
    FmInterpolation fmMethod{FmInterpolation::Sinc};
    LoadOptions effectiveOptions() const;
    VideoStandard effectiveVideo() const;
    void normalize();
};
struct AppSettings {
    bool fourThree{};
    RomDefaults rom;
    std::optional<ControllerConfiguration> controllers;
    ShortcutBindings shortcuts{defaultShortcuts};
};
struct RomIdentity {
    std::array<uint8_t,32> sha256{};
    std::array<uint8_t,20> sha1{};
    uint32_t crc32{},bytes{};
    bool operator==(const RomIdentity&) const=default;
};
std::variant<RomIdentity,DatabaseError> identifyRom(const RomImage& image);
struct RomSettings {
    Machine machine{Machine::Sms};
    VideoStandard video{VideoStandard::Ntsc};
    MapperKind mapper{MapperKind::Sega};
    uint32_t ramBytes{};
    bool fmEnabled{};
    std::filesystem::path fmPlugin;
    FmInterpolation interpolation{FmInterpolation::Sinc};
    LoadOptions loadOptions() const { return {mapper, ramBytes, CopierHeader::None, machine}; }
    bool operator==(const RomSettings&) const=default;
};
struct RomRecord {
    int64_t id{};
    RomIdentity identity;
    RomSettings settings;
    std::wstring name;
    std::filesystem::path lastPath;
    bool operator==(const RomRecord&) const=default;
};
struct Cheat {
    std::wstring name;
    unsigned address{};
    SearchWidth width{SearchWidth::Byte};
    uint32_t value{};
    bool enabled{}, always{};
    bool operator==(const Cheat&) const=default;
};
bool validCheat(Machine machine,const Cheat& cheat);
std::optional<DatabaseError> validateRomRegistration(const RomIdentity& identity,const RomSettings& settings,
    std::wstring_view name,const std::filesystem::path& path);
struct StateSummary {
    int64_t id{};
    std::wstring name;
    int64_t createdAt{},updatedAt{};
    unsigned slot{};
    bool operator==(const StateSummary&) const=default;
};
enum class StateDisposition { Keep, Convert, Delete };
struct StateImpact {
    StateSummary state;
    StateDisposition disposition;
    bool operator==(const StateImpact&) const=default;
};
struct RomUpdateImpact {
    RomRecord before;
    std::vector<StateImpact> states;
};
struct SavedState {
    int64_t id{};
    std::wstring name;
    int64_t createdAt{},updatedAt{};
    std::unique_ptr<EmulatorSystemState> state;
};
struct StateResizeContext {
    // Caller-matched ROM/current profile and already resolved backend; references outlive the resize call.
    const RomImage& rom;
    const StateProfile& profile;
    FmBackendPtr backend;
};
struct RomUpdateContext {
    const RomImage& rom;
    uint32_t sampleRate;
    std::span<const FmBackendPtr> backends;
    FmBackendPtr targetBackend;
};
struct RomUpdateCounts {size_t kept{},converted{},deleted{};};
class RomDatabase;
// Owns the prepared transaction. Destroy before its database; commit consumes the reservation.
class PreparedRomUpdate final {
public:
    ~PreparedRomUpdate();
    PreparedRomUpdate(PreparedRomUpdate&&) noexcept;
    PreparedRomUpdate(const PreparedRomUpdate&)=delete;
    PreparedRomUpdate& operator=(const PreparedRomUpdate&)=delete;
    const RomRecord& record() const noexcept;
    bool changesSystem() const noexcept;
    std::variant<std::vector<StateImpact>,DatabaseError> stateImpact() const;
    std::unique_ptr<EmulatorSystem> takeSystem() noexcept;
private:
    friend class RomDatabase;
    struct Data;
    explicit PreparedRomUpdate(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
};

// One owning thread. Callers finalize their statements before releasing the database.
class RomDatabase final {
public:
    static std::variant<RomDatabase,DatabaseError> open(const std::filesystem::path& absolutePath);
    ~RomDatabase();
    RomDatabase(const RomDatabase&)=delete;
    RomDatabase& operator=(const RomDatabase&)=delete;
    RomDatabase(RomDatabase&& other) noexcept;
    RomDatabase& operator=(RomDatabase&&)=delete;
    sqlite3* nativeHandle() const noexcept {return db_;}
    std::optional<DatabaseError> execute(const char* sql);
    std::variant<AppSettings,DatabaseError> readAppSettings();
    std::optional<DatabaseError> saveAppSettings(const RomDefaults& rom,const ControllerConfiguration& controllers,
        const ShortcutBindings& shortcuts=defaultShortcuts);
    std::optional<DatabaseError> saveAspectRatio(bool fourThree);
    std::variant<std::vector<Cheat>,DatabaseError> readCheats(int64_t romId,Machine machine);
    std::optional<DatabaseError> saveCheats(int64_t romId,Machine machine,std::span<const Cheat> cheats);
    std::variant<std::optional<RomRecord>,DatabaseError> findRom(const RomIdentity& identity);
    std::variant<RomRecord,DatabaseError> registerRom(const RomIdentity& identity,const RomSettings& settings,
        std::wstring_view displayName,const std::filesystem::path& lastPath);
    // romId/profile/snapshot belong to the caller's validated current ROM configuration.
    std::variant<int64_t,DatabaseError> saveState(int64_t romId,std::wstring_view name,
        const StateProfile& profile,const EmulatorSystemState& snapshot,std::optional<int64_t> stateId={});
    // Restore the returned snapshot into an independent candidate before exchanging the live System.
    std::variant<SavedState,DatabaseError,StateCodecError> readState(int64_t romId,int64_t stateId,const StateProfile& profile);
    std::variant<std::vector<StateSummary>,DatabaseError> listStates(int64_t romId);
    std::optional<DatabaseError> deleteState(int64_t romId,int64_t stateId);
    std::variant<int64_t,DatabaseError> saveSlot(int64_t romId,unsigned slot,
        const StateProfile& profile,const EmulatorSystemState& snapshot);
    std::optional<DatabaseError> renameState(int64_t romId,int64_t stateId,std::wstring_view name);
private:
    friend class DbSramStore;
    std::variant<PreparedRomUpdate,DatabaseError> prepareUpdate(const RomRecord& before,std::span<const uint8_t> current,
        const RomSettings& settings,std::wstring_view name,const std::filesystem::path& path,const RomUpdateContext* context);
    std::variant<RomUpdateCounts,DatabaseError> commitUpdate(PreparedRomUpdate& prepared,RomRecord& record,
        std::vector<uint8_t>& current,std::vector<uint8_t>& baseline);
    std::variant<std::vector<uint8_t>,DatabaseError> readSram(int64_t romId,size_t expectedBytes);
    std::optional<DatabaseError> writeSram(int64_t romId,std::span<const uint8_t> current);
    std::optional<DatabaseError> resizeSram(int64_t romId,size_t oldBytes,std::span<const uint8_t> next,const StateResizeContext* context);
    explicit RomDatabase(sqlite3* db):db_(db) {}
    sqlite3* db_{};
};
}
