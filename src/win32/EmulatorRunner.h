#pragma once

#include "core/EmulatorSystem.h"
#include "core/cartridge/RomLoader.h"
#include "core/state/StateCodec.h"
#include "win32/AudioStream.h"
#include "win32/RomDatabase.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace smsgg::win32 {
enum class RunnerState { Idle, Loading, Running, Paused, Faulted, Stopped };
enum class RunnerOperation { None, LoadRom, SaveState, LoadState, SaveSram, PrepareClose, UpdateRom, ListStates, DeleteState, PreviewRomUpdate, ClearSram, CaptureCheats, SaveCheats, ApplyCheat };
struct RomRegistration {
    RomIdentity identity;
    RomSettings settings;
    std::wstring name;
};
struct UnregisteredRom {
    RomIdentity identity;
    RomMetadata metadata;
    CopierHeader copierHeader{};
    std::filesystem::path archiveEntry;
};
struct LoadRequest {
    std::filesystem::path path;
    LoadOptions options{MapperKind::Sega};
    VideoStandard standard{VideoStandard::Ntsc};
    std::optional<Machine> machine{Machine::Sms};
    bool fmEnabled{};
    FmInterpolation fmMethod{FmInterpolation::Sinc};
    std::filesystem::path fmPlugin;
    bool discardSram{};
    std::filesystem::path databasePath;
    std::optional<RomRegistration> registration;
    bool useDefaultDatabase{};

    bool usesDatabase() const { return useDefaultDatabase || !databasePath.empty(); }
};
struct RomUpdateRequest {
    std::optional<RomSettings> settings;
    std::wstring name;
    std::filesystem::path lastPath;
    std::vector<std::filesystem::path> sourcePlugins;
    std::optional<RomUpdateImpact> confirmedImpact;
    bool preview{};
};
struct CheatSnapshot {
    std::vector<uint8_t> ram;
    std::vector<Cheat> definitions;
    bool wasPaused{};
};
struct RunnerSnapshot {
    RunnerState state{RunnerState::Idle};
    Machine machine{Machine::Sms};
    bool fmEnabled{};
    MapperKind mapper{MapperKind::Sega};
    std::optional<RomMetadata> romMetadata;
    DisplayArea displayArea{0,0,256,192};
    uint64_t tstates{}, frames{};
    std::filesystem::path romPath, archiveEntry;
    std::array<Rgba8, 256 * 192> pixels{};
    bool frameChanged{}, romChanged{};
    bool muted{}, fastForward{}, fatal{};
    RunnerOperation operation{RunnerOperation::None};
    bool busy{}, closeReady{}, sramSaveFailed{}, sramEnabled{};
    bool operationSucceeded{};
    uint32_t ramBytes{};
    uint64_t operationSerial{};
    std::filesystem::path sramPath;
    std::optional<FmStateProfile> fmProfile;
    std::optional<RomRecord> databaseRecord;
    std::filesystem::path databasePath;
    VideoStandard video{VideoStandard::Ntsc};
    uint32_t sampleRate{};
    std::optional<UnregisteredRom> unregisteredRom;
    std::optional<RomUpdateCounts> romUpdate;
    std::optional<RomUpdateImpact> romUpdateImpact;
    std::optional<int64_t> savedStateId;
    std::optional<std::vector<StateSummary>> databaseStates;
    std::optional<CheatSnapshot> cheatSnapshot;
    std::string fmBackendName;
    uint64_t generated{}, enqueued{}, discardedQueued{}, discardedPending{};
    size_t queued{}, pending{};
    AudioStatus audio;
    std::string error;
};

// UI calls this object; only its worker ever touches the EmulatorSystem.
class EmulatorRunner final {
public:
    explicit EmulatorRunner(AudioStream& audio);
    ~EmulatorRunner();
    bool load(LoadRequest request);
    bool saveState(std::filesystem::path path);
    bool loadState(std::filesystem::path path);
    bool saveDatabaseState(std::wstring name,std::optional<int64_t> stateId=std::nullopt);
    bool saveDatabaseSlot(unsigned slot);
    bool renameDatabaseState(int64_t stateId,std::wstring name);
    bool loadDatabaseState(int64_t stateId);
    bool listDatabaseStates();
    bool deleteDatabaseState(int64_t stateId);
    bool saveSram();
    bool clearDatabaseSram();
    bool captureCheats();
    bool saveCheats(std::vector<Cheat> cheats);
    bool applyCheat(Cheat cheat);
    bool prepareClose(bool discardSram=false);
    bool updateRom(RomUpdateRequest request);
    void setPaused(bool paused);
    void setMuted(bool muted);
    void setFastForward(bool enabled);
    // Rapid bits are button 1 and 2 for player 1, followed by the same two bits for player 2.
    void setInput(PadState pad, bool consolePause = false, PadState player2 = {}, unsigned rapidButtons = 0);
    void clearInput();
    RunnerSnapshot takeSnapshot();
    HANDLE notificationEvent() const { return ready_.get(); } // Borrowed until destruction.
    void stop();
private:
    friend struct RunnerTestAccess;
    void run();
    void requestStop();
    void publishLocked(RunnerState state, const EmulatorSystem* system);
    void signalLocked();
    struct SaveState {std::filesystem::path path;};
    struct LoadState {std::filesystem::path path;};
    struct SaveDatabaseState {std::wstring name;std::optional<int64_t> stateId;unsigned slot{};bool rename{};};
    struct LoadDatabaseState {int64_t stateId;};
    struct ListDatabaseStates {};
    struct DeleteDatabaseState {int64_t stateId;};
    struct SaveSram {};
    struct ClearSram {};
    struct CaptureCheats {};
    struct SaveCheats {std::vector<Cheat> definitions;};
    struct ApplyCheat {Cheat definition;};
    struct PrepareClose {bool discardSram;};
    using Request=std::variant<LoadRequest,SaveState,LoadState,SaveSram,PrepareClose,RomUpdateRequest,
        SaveDatabaseState,LoadDatabaseState,ListDatabaseStates,DeleteDatabaseState,ClearSram,CaptureCheats,SaveCheats,ApplyCheat>;
    bool submit(Request request,RunnerOperation operation);
    std::unique_ptr<void, decltype(&::CloseHandle)> ready_;
    std::mutex mutex_;
    AudioStream& audio_;
    bool stopping_{}, paused_{}, muted_{}, fastForwardRequested_{}, consolePause_{};
    uint64_t generated_{}, enqueued_{}, discardedQueued_{}, discardedPending_{};
    size_t pending_{};
    PadState input_{};
    PadState player2_{};
    unsigned rapidHeld_{}, rapidPressed_{};
    std::optional<Request> request_;
    RunnerSnapshot latest_;
    std::thread worker_;
};
} // namespace smsgg::win32
