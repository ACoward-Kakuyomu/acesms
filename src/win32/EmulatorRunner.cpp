#include "win32/EmulatorRunner.h"
#include "win32/EmulationSession.h"
#include "win32/FmPlugin.h"
#include "win32/DbSramStore.h"
#include "win32/DatabasePath.h"
#include "win32/ZipRom.h"

#include <algorithm>
#include <system_error>
#include <stdexcept>
#include <utility>

namespace smsgg::win32 {
namespace {
struct RunnerDatabase {
    RomDatabase database;
    RomRecord record;
    DbSramStore sram;
    std::vector<Cheat> cheats;
    RunnerDatabase(RomDatabase db,RomRecord rom):database(std::move(db)),record(std::move(rom)),sram(database,record) {}
};
std::variant<FmBackendPtr,std::string> resolveBackend(bool enabled,const std::filesystem::path& plugin) {
    if(!enabled) return FmBackendPtr{};
    if(plugin.empty()) return FmBackendDefinition::builtin();
    return loadFmPlugin(fmPluginDirectory(),plugin);
}
struct DatabaseLoad {
    std::unique_ptr<RunnerDatabase> database;
    std::unique_ptr<EmulationSession> session;
    std::string backendName;
    std::optional<RomDatabase> registrationDatabase;
    std::filesystem::path path;
    std::filesystem::path archiveEntry;
};
const char* romLoadMessage(RomLoadError error) {
    constexpr std::array messages{"Empty ROM","Unsupported ROM size","Incompatible ROM options",
        "Invalid copier header","Cannot open ROM","Cannot determine ROM size","Cannot read ROM"};
    return messages[static_cast<size_t>(error)];
}
std::variant<DatabaseLoad,UnregisteredRom,std::string> prepareDatabaseLoad(const LoadRequest& request,uint32_t rate) {
    std::optional<ZipRom> zip;
    if(isZipRomPath(request.path)) {
        auto opened=readZipRom(request.path);
        if(auto* error=std::get_if<std::string>(&opened)) return std::move(*error);
        zip=std::get<ZipRom>(std::move(opened));
    }
    const auto attempt=[&](LoadOptions options) {
        return zip ? RomLoader::fromBytes(zip->bytes,options) : RomLoader::load(request.path,options);
    };
    const auto loadForIdentity = [&](CopierHeader prefix) {
        auto bytes = attempt({MapperKind::Auto, 0, prefix});
        const auto* error = std::get_if<RomLoadError>(&bytes);
        if (error && *error == RomLoadError::UnsupportedRomSize) {
            // 40 KiB is a valid SG-1000 layout. The database or registration still chooses the machine.
            return attempt({MapperKind::RomOnly, 0, prefix, Machine::Sg1000});
        }
        return bytes;
    };
    auto header=request.options.copierHeader;
    auto loaded=loadForIdentity(header);
    if(!request.registration) {
        if(auto* error=std::get_if<RomLoadError>(&loaded);
            error && (*error==RomLoadError::UnsupportedRomSize || *error==RomLoadError::InvalidCopierHeader)) {
            header=header==CopierHeader::None?CopierHeader::Strip512:CopierHeader::None;
            loaded=loadForIdentity(header);
        }
    }
    if(const auto* error=std::get_if<RomLoadError>(&loaded)) {
        if(zip) return zipRomError(request.path,zip->entry,romLoadMessage(*error));
        return std::string("Cannot read/normalize DB ROM");
    }
    auto& raw=std::get<RomImage>(loaded);auto identity=identifyRom(raw);
    if(auto* error=std::get_if<DatabaseError>(&identity)) return error->message;
    const auto& key=std::get<RomIdentity>(identity);
    if(request.registration && request.registration->identity!=key) {
        if(zip) return zipRomError(request.path,zip->entry,"ROM changed since registration lookup");
        return std::string("ROM changed since registration lookup");
    }
    auto resolvedPath=resolveDatabasePath(request.databasePath);
    if(auto* error=std::get_if<std::string>(&resolvedPath)) return *error;
    const auto& path=std::get<std::filesystem::path>(resolvedPath);
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(),directoryError);
    if(directoryError) {
        const auto utf8=path.u8string();
        return "Cannot create database directory for "+std::string(utf8.begin(),utf8.end())+
            " (Win32 "+std::to_string(directoryError.value())+")";
    }
    auto opened=RomDatabase::open(path);if(auto* error=std::get_if<DatabaseError>(&opened)) return error->message;
    auto& db=std::get<RomDatabase>(opened);auto found=db.findRom(key);
    if(auto* error=std::get_if<DatabaseError>(&found)) return error->message;
    auto& record=std::get<std::optional<RomRecord>>(found);
    if(!record && !request.registration)
        return UnregisteredRom{key,raw.metadata(),header,zip?std::move(zip->entry):std::filesystem::path{}};
    const auto& settings=record?record->settings:request.registration->settings;
    if(!record) {
        if(auto error=validateRomRegistration(key,settings,request.registration->name,request.path)) return error->message;
    }
    auto backend=resolveBackend(settings.fmEnabled,settings.fmPlugin);
    if(auto* error=std::get_if<std::string>(&backend)) return *error;
    const auto bytes=raw.bytes();auto image=std::get<RomImage>(RomLoader::fromBytes({bytes.begin(),bytes.end()},settings.loadOptions()));
    DatabaseLoad result;
    if(zip) result.archiveEntry=std::move(zip->entry);
    result.session=std::make_unique<EmulationSession>(std::move(image),request.path,settings.video,rate,settings.machine,settings.fmEnabled,settings.interpolation,std::get<FmBackendPtr>(backend));
    if(settings.fmEnabled) result.backendName=std::get<FmBackendPtr>(backend)->api().descriptor.name;
    if(record) result.database=std::make_unique<RunnerDatabase>(std::move(db),std::move(*record));
    else result.registrationDatabase.emplace(std::move(db));
    result.path=std::get<std::filesystem::path>(std::move(resolvedPath));
    return result;
}
std::string loadDatabaseSession(RunnerDatabase& database,EmulationSession& session) {
    auto cheats=database.database.readCheats(database.record.id,database.record.settings.machine);
    if(auto* reason=std::get_if<DatabaseError>(&cheats)) return reason->message;
    database.cheats=std::get<std::vector<Cheat>>(std::move(cheats));
    if(!database.sram.enabled()) return {};
    auto ram=database.sram.load();
    if(auto* reason=std::get_if<DatabaseError>(&ram)) return reason->message;
    auto state=std::make_unique<EmulatorSystemState>();
    session.system().captureState(*state);
    state->cartridge.ram=std::get<std::vector<uint8_t>>(std::move(ram));
    if(session.system().restoreState(*state)!=ACESMS_FM_OK) return "DB SRAM restore failed";
    return {};
}
std::string sessionError(const SessionError& error) {
    if(const auto* file=std::get_if<FileError>(&error)) {
        constexpr std::array stages{"open","size","read","write","flush","close","replace"};
        auto message=std::string("File ")+stages[static_cast<size_t>(file->operation)]+" failed: Win32 "+std::to_string(file->code);
        if(file->cleanupCode) message+="; temporary cleanup: Win32 "+std::to_string(file->cleanupCode);
        return message;
    }
    if(const auto* state=std::get_if<StateCodecError>(&error)) {
        constexpr std::array reasons{"format","size","checksum","ROM identity","session profile","value"};
        return std::string("State rejected: ")+reasons[static_cast<size_t>(*state)];
    }
    return "FM state failed: "+std::to_string(std::get<acesms_fm_result>(error));
}
}
EmulatorRunner::EmulatorRunner(AudioStream& audio)
    : ready_(CreateEventW(nullptr, TRUE, TRUE, nullptr), &::CloseHandle), audio_(audio) {
    if (!ready_) throw std::system_error(GetLastError(), std::system_category(), "CreateEventW");
    worker_ = std::thread(&EmulatorRunner::run, this);
}

EmulatorRunner::~EmulatorRunner() { stop(); }

bool EmulatorRunner::load(LoadRequest request) {
    if(request.registration && !request.usesDatabase()) return false;
    return submit(std::move(request),RunnerOperation::LoadRom);
}
bool EmulatorRunner::saveState(std::filesystem::path path) {return submit(SaveState{std::move(path)},RunnerOperation::SaveState);}
bool EmulatorRunner::loadState(std::filesystem::path path) {return submit(LoadState{std::move(path)},RunnerOperation::LoadState);}
bool EmulatorRunner::saveDatabaseState(std::wstring name,std::optional<int64_t> stateId) {
    return submit(SaveDatabaseState{std::move(name),stateId},RunnerOperation::SaveState);
}
bool EmulatorRunner::loadDatabaseState(int64_t stateId) {
    return submit(LoadDatabaseState{stateId},RunnerOperation::LoadState);
}
bool EmulatorRunner::saveDatabaseSlot(unsigned slot) {
    if(slot<1 || slot>10) return false;
    return submit(SaveDatabaseState{{},{},slot},RunnerOperation::SaveState);
}
bool EmulatorRunner::renameDatabaseState(int64_t stateId,std::wstring name) {
    return submit(SaveDatabaseState{std::move(name),stateId,0,true},RunnerOperation::SaveState);
}
bool EmulatorRunner::listDatabaseStates() {
    return submit(ListDatabaseStates{},RunnerOperation::ListStates);
}
bool EmulatorRunner::deleteDatabaseState(int64_t stateId) {
    return submit(DeleteDatabaseState{stateId},RunnerOperation::DeleteState);
}
bool EmulatorRunner::saveSram() {return submit(SaveSram{},RunnerOperation::SaveSram);}
bool EmulatorRunner::clearDatabaseSram() {return submit(ClearSram{},RunnerOperation::ClearSram);}
bool EmulatorRunner::captureCheats() {return submit(CaptureCheats{},RunnerOperation::CaptureCheats);}
bool EmulatorRunner::saveCheats(std::vector<Cheat> cheats) {return submit(SaveCheats{std::move(cheats)},RunnerOperation::SaveCheats);}
bool EmulatorRunner::applyCheat(Cheat cheat) {return submit(ApplyCheat{std::move(cheat)},RunnerOperation::ApplyCheat);}
bool EmulatorRunner::prepareClose(bool discardSram) {return submit(PrepareClose{discardSram},RunnerOperation::PrepareClose);}
bool EmulatorRunner::updateRom(RomUpdateRequest request) {
    const auto operation=request.preview?RunnerOperation::PreviewRomUpdate:RunnerOperation::UpdateRom;
    return submit(std::move(request),operation);
}

bool EmulatorRunner::submit(Request request,RunnerOperation operation) {
    {
        std::lock_guard lock(mutex_);
        if(stopping_ || latest_.fatal || latest_.busy || latest_.closeReady) return false;
        if(operation!=RunnerOperation::LoadRom && operation!=RunnerOperation::PrepareClose && !latest_.romMetadata) return false;
        if(operation==RunnerOperation::SaveState && latest_.state==RunnerState::Faulted) return false;
        if(operation==RunnerOperation::SaveSram && !latest_.sramEnabled) return false;
        if(operation==RunnerOperation::ClearSram && (!latest_.databaseRecord || !latest_.sramEnabled)) return false;
        if(operation==RunnerOperation::CaptureCheats && (!latest_.databaseRecord ||
                (latest_.state!=RunnerState::Running && latest_.state!=RunnerState::Paused))) return false;
        if((operation==RunnerOperation::SaveCheats || operation==RunnerOperation::ApplyCheat) &&
                (!latest_.databaseRecord || !paused_ || latest_.state!=RunnerState::Paused)) return false;
        if((operation==RunnerOperation::UpdateRom || operation==RunnerOperation::PreviewRomUpdate || operation==RunnerOperation::ListStates ||
                operation==RunnerOperation::DeleteState || std::holds_alternative<SaveDatabaseState>(request) ||
                std::holds_alternative<LoadDatabaseState>(request)) && !latest_.databaseRecord) return false;
        if(latest_.databaseRecord && (std::holds_alternative<SaveState>(request) || std::holds_alternative<LoadState>(request))) return false;
        request_=std::move(request);latest_.busy=true;latest_.operation=operation;
        latest_.savedStateId.reset();
        latest_.databaseStates.reset();
        latest_.cheatSnapshot.reset();
        latest_.romUpdateImpact.reset();
        latest_.unregisteredRom.reset();
        latest_.sramSaveFailed=false;latest_.operationSucceeded=false;latest_.romUpdate.reset();latest_.error.clear();signalLocked();
    }
    audio_.wakeProducer();
    return true;
}

void EmulatorRunner::setPaused(bool paused) {
    {
        std::lock_guard lock(mutex_);
        paused_ = paused;
        consolePause_ = false;
    }
    audio_.wakeProducer();
}

void EmulatorRunner::setMuted(bool muted) {
    {std::lock_guard lock(mutex_);muted_=muted;}
    audio_.wakeProducer();
}

void EmulatorRunner::setFastForward(bool enabled) {
    {std::lock_guard lock(mutex_);fastForwardRequested_=enabled;}
    audio_.wakeProducer();
}

void EmulatorRunner::setInput(PadState pad, bool consolePause, PadState player2, unsigned rapidButtons) {
    std::lock_guard lock(mutex_);
    input_ = pad;
    player2_ = player2;
    rapidPressed_ |= rapidButtons & ~rapidHeld_;
    rapidHeld_ = rapidButtons;
    if (consolePause && !paused_ && latest_.state == RunnerState::Running) consolePause_ = true;
}

void EmulatorRunner::clearInput() {
    std::lock_guard lock(mutex_);
    input_ = {};
    player2_ = {};
    rapidHeld_ = rapidPressed_ = 0;
    consolePause_ = false;
}

RunnerSnapshot EmulatorRunner::takeSnapshot() {
    std::lock_guard lock(mutex_);
    auto snapshot = latest_;
    if (!ResetEvent(ready_.get())) throw std::system_error(GetLastError(), std::system_category(), "ResetEvent");
    latest_.frameChanged = latest_.romChanged = false;
    latest_.cheatSnapshot.reset();
    latest_.error.clear();
    return snapshot;
}

void EmulatorRunner::stop() {
    if (!worker_.joinable()) return;
    requestStop();
    worker_.join();
}

void EmulatorRunner::requestStop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    audio_.wakeProducer();
}

void EmulatorRunner::signalLocked() {
    if (!SetEvent(ready_.get())) throw std::system_error(GetLastError(), std::system_category(), "SetEvent");
}

void EmulatorRunner::publishLocked(RunnerState state, const EmulatorSystem* system) {
    latest_.state = state;
    latest_.muted=muted_;
    latest_.generated=generated_;latest_.enqueued=enqueued_;
    latest_.discardedQueued=discardedQueued_;latest_.discardedPending=discardedPending_;
    latest_.queued=audio_.pcm.queued();latest_.pending=pending_;latest_.audio=audio_.status();
    if (system) {
        latest_.displayArea=system->displayArea();
        latest_.tstates = system->tstates();
        latest_.frames = system->completedFrames();
    }
    signalLocked();
}

void EmulatorRunner::run() {
    std::unique_ptr<RunnerDatabase> database;
    std::unique_ptr<EmulationSession> session;
    EmulatorSystem* system=nullptr;
    bool faulted=false,started=false,fatal=false;
    size_t offset=0;
    auto wait=[&] {
        if(WaitForSingleObject(audio_.producerEvent(),INFINITE)!=WAIT_OBJECT_0)
            throw std::system_error(GetLastError(),std::system_category(),"Wait for audio demand");
    };
    auto quiesce=[&](bool muted) {
        const auto sequence=audio_.request(AudioCommand::Stopped,muted);
        for(;;) {
            const auto status=audio_.status();
            if(!status.error.empty() || status.exited) return false;
            if(status.acknowledged>=sequence) break;
            wait();
        }
        discardedQueued_+=audio_.pcm.queued();
        discardedPending_+=pending_;
        audio_.pcm.reset();pending_=0;
        offset=system?system->audioFrames().size():0;
        started=false;
        return true;
    };
    std::array<uint64_t,4> rapidStartFrames{};
    auto enqueuePending=[&] {
        const auto free=audio_.targetFrames()-audio_.pcm.queued();
        const auto count=audio_.pcm.push(system->audioFrames().subspan(offset,std::min(free,pending_)));
        offset+=count;pending_-=count;enqueued_+=count;
    };
    for(;;) {
        try {
            const auto audioStatus=audio_.status();
            if(!audioStatus.error.empty() || audioStatus.exited) {
                std::lock_guard lock(mutex_);
                fatal=latest_.fatal=true;
                latest_.error=audioStatus.error.empty()?"Audio consumer exited":audioStatus.error;
                publishLocked(RunnerState::Faulted,system);
                break;
            }
            std::unique_lock lock(mutex_);
            const bool muted=muted_;
            if(stopping_) {
                lock.unlock();quiesce(muted);break;
            }
            if(request_) {
                auto request=std::move(*request_);request_.reset();
                const auto previousState=latest_.state;
                const bool wasPaused=paused_;
                if(std::holds_alternative<LoadRequest>(request)) publishLocked(RunnerState::Loading,system);
                lock.unlock();
                std::string error,backendName;
                std::filesystem::path sramPath;
                std::filesystem::path nextDatabasePath, nextArchiveEntry;
                bool sramSaveFailed=false,closePrepared=false,exchanged=false;
                std::unique_ptr<RunnerDatabase> nextDatabase;
                std::unique_ptr<EmulationSession> nextSession;
                std::unique_ptr<EmulatorSystem> nextState;
                std::optional<int64_t> savedStateId;
                std::optional<std::vector<StateSummary>> databaseStates;
                std::optional<CheatSnapshot> cheatSnapshot;
                std::optional<std::vector<Cheat>> replacementCheats;
                std::optional<PreparedRomUpdate> update;
                std::optional<RomUpdateImpact> romUpdateImpact;
                std::optional<RomRecord> nextRecord;
                std::optional<UnregisteredRom> unregisteredRom;
                std::optional<RomDatabase> registrationDatabase;
                std::vector<uint8_t> currentRam;
                const auto saveRam=[&] {
                    sramSaveFailed=true;
                    if(database) {
                        auto state=std::make_unique<EmulatorSystemState>();system->captureState(*state);
                        if(auto reason=database->sram.save(state->cartridge.ram)) error=reason->message;
                        else sramSaveFailed=false;
                    } else if(auto reason=session->saveSram()) error=sessionError(*reason);
                    else sramSaveFailed=false;
                };
                try {
                    const auto* dbLoad=std::get_if<LoadRequest>(&request);
                    if(dbLoad && dbLoad->usesDatabase()) {
                        auto prepared=prepareDatabaseLoad(*dbLoad,audio_.sampleRate());
                        if(auto* reason=std::get_if<std::string>(&prepared)) error=*reason;
                        else if(auto* unknown=std::get_if<UnregisteredRom>(&prepared)) unregisteredRom=std::move(*unknown);
                        else {
                            auto& candidate=std::get<DatabaseLoad>(prepared);
                            nextDatabase=std::move(candidate.database);
                            nextSession=std::move(candidate.session);
                            backendName=std::move(candidate.backendName);
                            nextDatabasePath=std::move(candidate.path);
                            nextArchiveEntry=std::move(candidate.archiveEntry);
                            if(candidate.registrationDatabase) registrationDatabase.emplace(std::move(*candidate.registrationDatabase));
                            if(session && !dbLoad->discardSram) saveRam();
                            if(error.empty() && nextDatabase) {
                                error=loadDatabaseSession(*nextDatabase,*nextSession);
                                if(error.empty()) nextRecord=nextDatabase->record;
                            }
                        }
                    } else if(const auto* edit=std::get_if<RomUpdateRequest>(&request)) {
                        FmBackendPtr backend;
                        std::vector<FmBackendPtr> sources;
                        std::optional<RomUpdateContext> context;
                        if(edit->confirmedImpact && edit->confirmedImpact->before!=database->record)
                            error="ROM settings changed; review the changes again";
                        if(error.empty() && edit->settings) {
                            auto resolved=resolveBackend(edit->settings->fmEnabled,edit->settings->fmPlugin);
                            if(auto* reason=std::get_if<std::string>(&resolved)) error=*reason;
                            else backend=std::get<FmBackendPtr>(std::move(resolved));
                            sources={FmBackendDefinition::builtin(),session->fmBackend(),backend};
                            for(const auto& path:edit->sourcePlugins) {
                                if(!error.empty()) break;
                                auto source=resolveBackend(true,path);
                                if(auto* reason=std::get_if<std::string>(&source)) error=*reason;else sources.push_back(std::get<FmBackendPtr>(std::move(source)));
                            }
                            if(error.empty()) {
                                auto state=std::make_unique<EmulatorSystemState>();system->captureState(*state);currentRam=std::move(state->cartridge.ram);
                                context.emplace(RomUpdateContext{session->romImage(),audio_.sampleRate(),sources,backend});
                            }
                        }
                        if(error.empty()) {
                            const auto& settings=edit->settings?*edit->settings:database->record.settings;
                            auto prepared=database->sram.prepareUpdate(database->record,currentRam,settings,edit->name,edit->lastPath,context?&*context:nullptr);
                            if(auto* reason=std::get_if<DatabaseError>(&prepared)) error=reason->message;
                            else {
                                update.emplace(std::get<PreparedRomUpdate>(std::move(prepared)));
                                if(edit->preview || edit->confirmedImpact) {
                                    auto impact=update->stateImpact();
                                    if(auto* reason=std::get_if<DatabaseError>(&impact)) error=reason->message;
                                    else {
                                        auto& states=std::get<std::vector<StateImpact>>(impact);
                                        if(edit->preview) romUpdateImpact=RomUpdateImpact{database->record,std::move(states)};
                                        else if(states!=edit->confirmedImpact->states) error="Saved states changed; review the changes again";
                                    }
                                    if(edit->preview || !error.empty()) update.reset();
                                }
                                if(update) nextRecord=update->record();
                                if(update && update->changesSystem()) {
                                    auto cheats=database->database.readCheats(database->record.id,settings.machine);
                                    if(auto* reason=std::get_if<DatabaseError>(&cheats)) error=reason->message;
                                    else {
                                        replacementCheats=std::get<std::vector<Cheat>>(std::move(cheats));
                                        const auto bytes=session->romImage().bytes();
                                        auto image=std::get<RomImage>(RomLoader::fromBytes({bytes.begin(),bytes.end()},settings.loadOptions()));
                                        std::filesystem::path path;{std::lock_guard sourceLock(mutex_);path=latest_.romPath;}
                                        nextSession=std::make_unique<EmulationSession>(std::move(image),path,settings.video,audio_.sampleRate(),settings.machine,settings.fmEnabled,settings.interpolation,backend,update->takeSystem());
                                        if(backend) backendName=backend->api().descriptor.name;
                                    }
                                }
                            }
                        }
                    } else if(const auto* load=std::get_if<LoadRequest>(&request)) {
                        Machine machine=load->machine.value_or(Machine::Sms);
                        if(load->fmEnabled && (machine!=Machine::Sms || load->standard!=VideoStandard::Ntsc)) error="FM requires SMS NTSC";
                        else if(load->machine==Machine::GameGear && load->standard!=VideoStandard::Ntsc) error="Game Gear requires NTSC";
                        else if(machine==Machine::Sg1000 && load->standard!=VideoStandard::Ntsc) error="SG-1000 requires NTSC";
                        std::optional<RomImage> image;
                        if(error.empty()) {
                            auto options=load->options;
                            options.machine=machine;
                            auto loaded=RomLoader::load(load->path,options);
                            if(const auto* reason=std::get_if<RomLoadError>(&loaded)) error=romLoadMessage(*reason);
                            else {
                                image.emplace(std::get<RomImage>(std::move(loaded)));
                                if(!load->machine) {
                                    const auto& header=image->metadata().header;
                                    if(!header || header->model==RomModel::Unknown) error="Cannot determine SMS/Game Gear; select machine";
                                    else machine=header->model==RomModel::GameGear?Machine::GameGear:Machine::Sms;
                                }
                                if(error.empty() && machine==Machine::GameGear) {
                                    if(load->fmEnabled) error="FM requires SMS NTSC";
                                    else if(load->standard!=VideoStandard::Ntsc) error="Game Gear requires NTSC";
                                }
                            }
                        }
                        FmBackendPtr backend;
                        if(error.empty() && load->fmEnabled) {
                            if(load->fmPlugin.empty()) backend=FmBackendDefinition::builtin();
                            else {
                                auto loaded=loadFmPlugin(fmPluginDirectory(),load->fmPlugin);
                                if(auto* reason=std::get_if<std::string>(&loaded)) error=std::move(*reason);
                                else backend=std::get<FmBackendPtr>(std::move(loaded));
                            }
                            if(backend) backendName=backend->api().descriptor.name;
                        }
                        if(error.empty() && session && !load->discardSram) {
                            saveRam();
                        }
                        if(error.empty()) {
                            nextSession=std::make_unique<EmulationSession>(std::move(*image),load->path,load->standard,audio_.sampleRate(),machine,load->fmEnabled,load->fmMethod,std::move(backend));
                            if(auto reason=nextSession->loadSram()) {error=sessionError(*reason);nextSession.reset();}
                            else sramPath=nextSession->sramPath();
                        }
                    } else if(const auto* save=std::get_if<SaveState>(&request)) {
                        if(auto reason=session->saveState(save->path)) error=sessionError(*reason);
                    } else if(const auto* load=std::get_if<LoadState>(&request)) {
                        auto prepared=session->prepareState(load->path);
                        if(const auto* reason=std::get_if<SessionError>(&prepared)) error=sessionError(*reason);
                        else nextState=std::get<std::unique_ptr<EmulatorSystem>>(std::move(prepared));
                    } else if(const auto* save=std::get_if<SaveDatabaseState>(&request)) {
                        if(save->rename) {
                            if(auto reason=database->database.renameState(database->record.id,*save->stateId,save->name)) error=reason->message;
                        } else {
                            auto state=std::make_unique<EmulatorSystemState>();
                            system->captureState(*state);
                            auto saved=save->slot?database->database.saveSlot(database->record.id,save->slot,session->profile(),*state):
                                database->database.saveState(database->record.id,save->name,session->profile(),*state,save->stateId);
                            if(const auto* reason=std::get_if<DatabaseError>(&saved)) error=reason->message;
                            else savedStateId=std::get<int64_t>(saved);
                        }
                        auto listed=database->database.listStates(database->record.id);
                        if(auto* states=std::get_if<std::vector<StateSummary>>(&listed)) databaseStates=std::move(*states);
                        else if(error.empty()) error=std::get<DatabaseError>(listed).message;
                    } else if(const auto* load=std::get_if<LoadDatabaseState>(&request)) {
                        auto saved=database->database.readState(database->record.id,load->stateId,session->profile());
                        if(const auto* reason=std::get_if<DatabaseError>(&saved)) error=reason->message;
                        else if(const auto* reason=std::get_if<StateCodecError>(&saved)) error=sessionError(*reason);
                        else {
                            auto prepared=session->prepareState(*std::get<SavedState>(saved).state);
                            if(const auto* reason=std::get_if<SessionError>(&prepared)) error=sessionError(*reason);
                            else nextState=std::get<std::unique_ptr<EmulatorSystem>>(std::move(prepared));
                        }
                    } else if(std::holds_alternative<ListDatabaseStates>(request)) {
                        auto listed=database->database.listStates(database->record.id);
                        if(const auto* reason=std::get_if<DatabaseError>(&listed)) error=reason->message;
                        else databaseStates=std::get<std::vector<StateSummary>>(std::move(listed));
                    } else if(const auto* erase=std::get_if<DeleteDatabaseState>(&request)) {
                        if(auto reason=database->database.deleteState(database->record.id,erase->stateId)) error=reason->message;
                        auto listed=database->database.listStates(database->record.id);
                        if(auto* values=std::get_if<std::vector<StateSummary>>(&listed)) databaseStates=std::move(*values);
                    } else if(std::holds_alternative<ClearSram>(request)) {
                        if(auto reason=database->sram.clear(system->cartridgeRam())) error=reason->message;
                    } else if(std::holds_alternative<CaptureCheats>(request)) {
                        cheatSnapshot=CheatSnapshot{system->captureWorkRam(),database->cheats,wasPaused};
                        if(previousState!=RunnerState::Paused && !quiesce(muted)) error="Audio consumer did not acknowledge stop";
                    } else if(auto* save=std::get_if<SaveCheats>(&request)) {
                        if(auto reason=database->database.saveCheats(database->record.id,database->record.settings.machine,save->definitions)) error=reason->message;
                        else database->cheats=std::move(save->definitions);
                    } else if(const auto* apply=std::get_if<ApplyCheat>(&request)) {
                        const auto& cheat=apply->definition;
                        if(!cheat.enabled || cheat.always || !system->writeWorkRam(cheat.address,cheat.width,cheat.value))
                            error="Invalid single-use cheat";
                        else cheatSnapshot=CheatSnapshot{system->captureWorkRam(),{},true};
                    } else {
                        const auto* close=std::get_if<PrepareClose>(&request);
                        if(session && (!close || !close->discardSram)) {
                            saveRam();
                        }
                        closePrepared=close && error.empty();
                    }
                } catch(const std::exception& reason) {
                    // Request boundary: failed input/I/O/allocation leaves the active session running.
                    error=reason.what();nextSession.reset();nextState.reset();update.reset();closePrepared=false;
                }
                const bool needsStop=error.empty() && (nextSession || nextState || closePrepared);
                {
                    std::lock_guard checkStop(mutex_);
                    if(stopping_) continue;
                }
                if(needsStop) {
                    if(!quiesce(muted)) error="Audio consumer did not acknowledge stop";
                    std::lock_guard checkStop(mutex_);
                    if(stopping_) continue;
                }
                std::optional<RomUpdateCounts> committedUpdate;
                if(error.empty() && update) {
                    try {
                        auto committed=database->sram.commitUpdate(*update,database->record,currentRam);
                        if(auto* reason=std::get_if<DatabaseError>(&committed)) error=reason->message;
                        else committedUpdate=std::get<RomUpdateCounts>(committed);
                    } catch(const std::exception& reason) {error=reason.what();update.reset();}
                } else if(error.empty() && registrationDatabase) {
                    try {
                        const auto& load=std::get<LoadRequest>(request);
                        const auto& registration=*load.registration;
                        auto registered=registrationDatabase->registerRom(registration.identity,registration.settings,registration.name,load.path);
                        if(auto* reason=std::get_if<DatabaseError>(&registered)) error=reason->message;
                        else {
                            auto& record=std::get<RomRecord>(registered);
                            if(record.settings!=registration.settings) error="ROM was registered with different settings; reload ROM";
                            else {
                                nextDatabase=std::make_unique<RunnerDatabase>(std::move(*registrationDatabase),std::move(record));
                                error=loadDatabaseSession(*nextDatabase,*nextSession);
                                if(error.empty()) nextRecord=nextDatabase->record;
                            }
                        }
                    } catch(const std::exception& reason) {
                        error=reason.what();
                    }
                }
                auto* loadRequest=std::get_if<LoadRequest>(&request);
                auto* listDatabase=loadRequest && nextSession?nextDatabase.get():database.get();
                if(listDatabase && error.empty() && (loadRequest || std::holds_alternative<RomUpdateRequest>(request))) {
                    auto listed=listDatabase->database.listStates(listDatabase->record.id);
                    if(auto* values=std::get_if<std::vector<StateSummary>>(&listed)) databaseStates=std::move(*values);
                }

                lock.lock();
                if(stopping_) {lock.unlock();continue;}
                const bool pausedForCheats=error.empty() && std::holds_alternative<CaptureCheats>(request);
                if(pausedForCheats) {paused_=true;consolePause_=false;}
                if(committedUpdate) {
                    latest_.romUpdate=committedUpdate;
                    latest_.databaseRecord=std::move(nextRecord);
                }
                if(error.empty() && nextSession) {
                    exchanged=true;
                    session.swap(nextSession);system=&session->system();offset=0;faulted=false;input_={};player2_={};consolePause_=false;
                    rapidHeld_=rapidPressed_=0;
                    if(loadRequest) {
                        database.swap(nextDatabase);
                        latest_.databaseRecord=std::move(nextRecord);
                        latest_.databasePath=std::move(nextDatabasePath);
                        latest_.archiveEntry=std::move(nextArchiveEntry);
                    } else if(replacementCheats) database->cheats=std::move(*replacementCheats);
                    const auto& profile=session->profile();
                    latest_.machine=profile.machine;latest_.fmEnabled=profile.fm.has_value();latest_.fmProfile=profile.fm;
                    latest_.video=profile.video;latest_.sampleRate=profile.sampleRate;
                    latest_.fmBackendName=std::move(backendName);latest_.mapper=profile.mapper;latest_.romMetadata=session->metadata();
                    latest_.sramEnabled=database?database->sram.enabled():session->hasSram();latest_.ramBytes=profile.ramBytes;latest_.sramPath=std::move(sramPath);
                    if(std::holds_alternative<RomUpdateRequest>(request)) std::ranges::copy(system->framePixels(),latest_.pixels.begin());
                    else latest_.pixels.fill({});
                    latest_.frameChanged=latest_.romChanged=true;
                    if(loadRequest) latest_.romPath=std::move(loadRequest->path);
                } else if(error.empty() && nextState) {
                    exchanged=true;
                    nextState=session->commitState(std::move(nextState));system=&session->system();offset=0;faulted=false;input_={};player2_={};consolePause_=false;
                    rapidHeld_=rapidPressed_=0;
                    std::ranges::copy(system->framePixels(),latest_.pixels.begin());latest_.frameChanged=true;
                } else if(error.empty() && closePrepared) latest_.closeReady=true;
                latest_.savedStateId=savedStateId;
                latest_.databaseStates=std::move(databaseStates);
                if(error.empty()) latest_.cheatSnapshot=std::move(cheatSnapshot);
                latest_.romUpdateImpact=std::move(romUpdateImpact);
                latest_.unregisteredRom=std::move(unregisteredRom);
                latest_.busy=false;++latest_.operationSerial;latest_.sramSaveFailed=sramSaveFailed;latest_.operationSucceeded=error.empty() && !latest_.unregisteredRom;latest_.error=std::move(error);
                // Ordinary saves leave pause transitions to the existing quiesce path.
                const auto state=pausedForCheats?RunnerState::Paused:
                    exchanged?(paused_?RunnerState::Paused:RunnerState::Running):previousState;
                publishLocked(state,system);lock.unlock();continue;
            }
            if(latest_.closeReady) {lock.unlock();wait();continue;}
            if(latest_.fastForward!=fastForwardRequested_) {
                if(started || pending_ || audio_.pcm.queued()) {
                    lock.unlock();
                    if(!quiesce(muted)) continue;
                    lock.lock();
                }
                latest_.fastForward=fastForwardRequested_;
                publishLocked(latest_.state,system);
                continue;
            }
            if(!system || faulted || paused_) {
                if(system && !faulted && latest_.state!=RunnerState::Paused) {
                    consolePause_=false;lock.unlock();
                    if(!quiesce(muted)) continue;
                    lock.lock();publishLocked(RunnerState::Paused,system);
                    continue; // The stop ack wait may have consumed a wake for a newer request.
                }
                audio_.request(AudioCommand::Stopped,muted);
                if(latest_.muted!=muted) publishLocked(latest_.state,system);
                lock.unlock();wait();continue;
            }
            if(latest_.state!=RunnerState::Running) publishLocked(RunnerState::Running,system);
            auto pad=input_;
            auto player2=player2_;
            const auto rapidPressed=std::exchange(rapidPressed_,0u);
            if(rapidHeld_) {
                const auto frame=system->completedFrames();
                const std::array buttons{&pad.button1,&pad.button2,&player2.button1,&player2.button2};
                for(size_t i=0;i<buttons.size();++i) {
                    const auto bit=1u<<i;
                    if(rapidPressed&bit) rapidStartFrames[i]=frame;
                    if(rapidHeld_&bit) *buttons[i]|=((frame-rapidStartFrames[i])%4)<2;
                }
            }
            const bool fastForward=latest_.fastForward;
            bool takePause=true;
            lock.unlock();
            if(started) audio_.request(AudioCommand::Playing,muted);
            for(unsigned batch=0;batch<256;++batch) {
                if(pending_) enqueuePending();
                if(!fastForward && audio_.pcm.queued()>=audio_.targetFrames()) {
                    audio_.request(AudioCommand::Playing,muted);started=true;
                    audio_.wakeConsumer();
                    {std::lock_guard publish(mutex_);publishLocked(RunnerState::Running,system);}
                    wait();break;
                }
                if(pending_) continue;
                bool pause=false;
                if(takePause) {
                    std::lock_guard inputLock(mutex_);
                    pause=std::exchange(consolePause_,false);takePause=false;
                }
                const auto frame=system->completedFrames();
                const auto elapsed=system->step(pad,pause,player2);
                if(std::holds_alternative<CpuStepError>(elapsed)) {
                    if(!quiesce(muted)) break;
                    std::lock_guard publish(mutex_);faulted=true;
                    latest_.error="Execution stopped: 65536 consecutive DD/FD prefixes";
                    publishLocked(RunnerState::Faulted,system);break;
                }
                offset=0;pending_=system->audioFrames().size();generated_+=pending_;
                if(fastForward && pending_) {
                    enqueuePending();
                    discardedPending_+=pending_;
                    pending_=0;
                    if(!started && audio_.pcm.queued()>=audio_.targetFrames()) {
                        audio_.request(AudioCommand::Playing,muted);started=true;
                    }
                }
                if(system->completedFrames()!=frame) {
                    if(database) for(const auto& cheat:database->cheats) {
                        if(cheat.enabled && cheat.always) system->writeWorkRam(cheat.address,cheat.width,cheat.value);
                    }
                    std::lock_guard publish(mutex_);
                    std::ranges::copy(system->framePixels(),latest_.pixels.begin());latest_.frameChanged=true;
                    publishLocked(RunnerState::Running,system);break;
                }
            }
        } catch(const std::exception& error) {
            // Worker boundary: stop audio before exposing a recoverable CPU/host fault.
            bool muted;
            {std::lock_guard lock(mutex_);muted=muted_;}
            if(!quiesce(muted)) continue;
            std::lock_guard lock(mutex_);faulted=true;latest_.error=error.what();
            publishLocked(RunnerState::Faulted,system);
        }
    }
    std::lock_guard lock(mutex_);
    latest_.busy=false;request_.reset();
    publishLocked(fatal?RunnerState::Faulted:RunnerState::Stopped,system);
}
} // namespace smsgg::win32
