#include "win32/EmulatorRunner.h"
#include "win32/DatabasePath.h"
#include "win32/EmulationSession.h"
#include "win32/RomDatabase.h"
#include "FakeAudioConsumer.h"
#include "ZipRomTestSupport.h"
#include "../system/Stage5TestRom.h"
#include "../system/Sg1000TestRom.h"
#include <sqlite3.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace smsgg::win32 {
struct RunnerTestAccess {
    static void requestStop(EmulatorRunner& r) {r.requestStop();}
    static bool pressed(EmulatorRunner& r) {std::lock_guard lock(r.mutex_);return r.input_.button1;}
};
}
namespace {
using namespace smsgg;using namespace smsgg::win32;
void require(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
template<class Predicate> std::unique_ptr<RunnerSnapshot> waitFor(EmulatorRunner& r,Predicate predicate) {
    const auto end=GetTickCount64()+10000;
    for(;;) {
        auto s=std::make_unique<RunnerSnapshot>(r.takeSnapshot());if(predicate(*s)) return s;
        const auto now=GetTickCount64();require(now<end,"runner timeout");
        require(WaitForSingleObject(r.notificationEvent(),static_cast<DWORD>(end-now))==WAIT_OBJECT_0,"runner event timeout");
    }
}
std::unique_ptr<RunnerSnapshot> done(EmulatorRunner& r,uint64_t serial) {return waitFor(r,[&](const auto& s){return !s.busy && s.operationSerial>serial;});}
using Raw=std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)>;
Raw raw(const std::filesystem::path& path) {sqlite3* p=nullptr;const auto code=sqlite3_open16(path.c_str(),&p);Raw db(p,sqlite3_close_v2);require(code==SQLITE_OK,"raw DB");return db;}
void sql(sqlite3* db,const char* text) {require(sqlite3_exec(db,text,nullptr,nullptr,nullptr)==SQLITE_OK,"fixture SQL");}
std::string fileBytes(const std::filesystem::path& path) {std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
std::vector<uint8_t> sram(const std::filesystem::path& path) {
    auto db=raw(path);sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db.get(),"SELECT data FROM sram",-1,&s,nullptr)==SQLITE_OK,"SRAM prepare");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owner(s,sqlite3_finalize);require(sqlite3_step(s)==SQLITE_ROW,"SRAM row");
    const auto* p=static_cast<const uint8_t*>(sqlite3_column_blob(s,0));return {p,p+sqlite3_column_bytes(s,0)};
}
LoadRequest fixture(const std::filesystem::path& dir,bool fm=false) {
    LoadRequest request;request.path=dir/L"cart.sms";request.databasePath=dir/L"roms.sqlite3";
    std::filesystem::remove(request.databasePath);auto sav=request.path;sav.replace_extension(L".sav");std::filesystem::remove(sav);
    auto bytes=test::stage5InputRom();bytes.resize(32768);bytes[1]=0;bytes[2]=8;
    const uint8_t code[]{0x3e,8,0x32,0xfc,0xff,0x3e,0x5a,0x32,0,0xa0,0xc3,0,1};std::copy(std::begin(code),std::end(code),bytes.begin()+0x800);
    {std::ofstream f(request.path,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(f),"ROM write");}
    auto image=std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::Sega,8192}));
    auto opened=RomDatabase::open(request.databasePath);require(std::holds_alternative<RomDatabase>(opened),"seed DB");auto& db=std::get<RomDatabase>(opened);
    RomSettings settings;settings.ramBytes=8192;settings.fmEnabled=fm;
    auto registered=db.registerRom(std::get<RomIdentity>(identifyRom(image)),settings,L"old",request.path);require(std::holds_alternative<RomRecord>(registered),"register");
    EmulationSession session(image,request.path,VideoStandard::Ntsc,48000,Machine::Sms,fm);
    auto state=std::make_unique<EmulatorSystemState>();session.system().captureState(*state);state->cartridge.ram[0]=0x44;
    require(std::holds_alternative<int64_t>(db.saveState(std::get<RomRecord>(registered).id,L"state",session.profile(),*state)),"state seed");
    return request;
}
std::unique_ptr<RunnerSnapshot> load(EmulatorRunner& r,const LoadRequest& request) {
    r.setPaused(true);require(r.load(request),"load submit");auto s=done(r,0);
    require(s->operationSucceeded && s->error.empty() && s->databaseRecord && s->ramBytes==8192 && s->sramPath.empty(),"DB load");return s;
}
std::unique_ptr<RunnerSnapshot> dirty(EmulatorRunner& r) {
    r.setPaused(false);waitFor(r,[](const auto& s){return s.frames>=2;});r.setPaused(true);return waitFor(r,[](const auto& s){return s.state==RunnerState::Paused;});
}
RomUpdateRequest change(const RunnerSnapshot& s,uint32_t bytes=16384) {
    RomUpdateRequest request;request.settings=s.databaseRecord->settings;request.settings->ramBytes=bytes;request.name=L"updated";request.lastPath=s.romPath;return request;
}
struct HoldStop {
    test::FakeAudioConsumer& audio;
    explicit HoldStop(test::FakeAudioConsumer& a):audio(a) {audio.holdStop(true);}
    ~HoldStop(){audio.holdStop(false);}
    void release(){audio.holdStop(false);}
};
void awaitStop(test::FakeAudioConsumer& a,uint64_t old) {
    const auto end=GetTickCount64()+10000;
    while(a.stream.control().sequence<=old && GetTickCount64()<end) Sleep(1);
    const auto c=a.stream.control();require(c.command==AudioCommand::Stopped && c.sequence>old && a.stream.status().acknowledged<c.sequence,"missing held stop");
}

struct CommitPause {
    std::unique_ptr<void,decltype(&CloseHandle)> entered{CreateEventW(nullptr,TRUE,FALSE,nullptr),CloseHandle};
    std::unique_ptr<void,decltype(&CloseHandle)> release{CreateEventW(nullptr,TRUE,FALSE,nullptr),CloseHandle};
    std::atomic<bool> armed{};
    static inline CommitPause* active{};

    static int install(sqlite3* db, char**, const sqlite3_api_routines*) {
        sqlite3_commit_hook(db, [](void* user) {
            auto& pause = *static_cast<CommitPause*>(user);
            if (!pause.armed.exchange(false)) return 0;
            if (!SetEvent(pause.entered.get())) return 1;
            return WaitForSingleObject(pause.release.get(), 10000) == WAIT_OBJECT_0 ? 0 : 1;
        }, active);
        return SQLITE_OK;
    }
    CommitPause() {
        require(entered && release, "create SQL commit events");
        require(sqlite3_auto_extension(reinterpret_cast<void(*)()>(install)) == SQLITE_OK, "install SQL commit hook");
        active = this;
    }
    ~CommitPause() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void(*)()>(install));
        active = nullptr;
    }
};

void commitResponsiveness(const std::filesystem::path& root) {
    for (const bool stopInFlight : {false, true}) {
        const auto dir = root / (stopInFlight ? "stop" : "publish");
        std::filesystem::create_directories(dir);
        const auto request = fixture(dir);
        CommitPause commit;
        test::FakeAudioConsumer audio(false);
        EmulatorRunner runner(audio.stream);
        auto old = load(runner, request);
        runner.setPaused(false);
        old = waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
        const auto sequence = audio.stream.control().sequence;
        HoldStop hold(audio);
        require(runner.updateRom(change(*old)), "submit update with held commit");
        awaitStop(audio, sequence);
        commit.armed = true;
        hold.release();
        require(WaitForSingleObject(commit.entered.get(), 10000) == WAIT_OBJECT_0, "worker did not enter commit");
        auto ui = std::async(std::launch::async, [&] {
            runner.setInput({.button1=true});
            auto snapshot = std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
            if (stopInFlight) RunnerTestAccess::requestStop(runner);
            return snapshot;
        });
        const auto ready = ui.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        SetEvent(commit.release.get());
        auto pending = ui.get();
        require(ready, "commit blocked UI input, snapshot, or stop");
        require(pending->busy && pending->operationSerial==old->operationSerial &&
            pending->databaseRecord==old->databaseRecord && pending->ramBytes==8192, "published before DB commit");
        if (stopInFlight) {
            runner.stop();
            const auto stopped = std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
            require(stopped->state==RunnerState::Stopped && stopped->databaseRecord==old->databaseRecord &&
                stopped->ramBytes==8192, "published a candidate after stop was requested");
        } else {
            const auto updated = done(runner, old->operationSerial);
            require(updated->operationSucceeded && updated->romChanged && updated->databaseRecord->settings.ramBytes==16384 &&
                updated->ramBytes==16384 && updated->databaseStates && updated->databaseStates->size()==1,
                "commit and snapshot publication did not agree");
        }
        require(sram(request.databasePath).size()==16384, "successful commit did not persist");
    }
}
void loading(const std::filesystem::path& dir) {
    const auto request=fixture(dir);test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);
    auto ignored=request;ignored.machine=Machine::GameGear;ignored.standard=VideoStandard::Pal;ignored.fmEnabled=true;
    auto s=load(runner,ignored);require(s->machine==Machine::Sms && !s->fmEnabled && s->sramEnabled,"load did not use DB settings");
    require(!runner.saveState(dir/L"external.state") && !runner.loadState(dir/L"external.state"),"DB mode accepted file state I/O");
    s=dirty(runner);require(sram(request.databasePath)[0]==0,"dirty SRAM saved early");
    require(runner.load(request),"DB reload");s=done(runner,s->operationSerial);require(s->operationSucceeded && s->tstates==0 && sram(request.databasePath)[0]==0x5a,"same ROM DB SRAM order");
    require(runner.saveSram(),"DB save");s=done(runner,s->operationSerial);require(s->operationSucceeded && sram(request.databasePath)[0]==0x5a,"DB SRAM save");
    require(runner.prepareClose(),"DB close");s=done(runner,s->operationSerial);require(s->closeReady,"DB close failed");runner.stop();
    auto sav=request.path;sav.replace_extension(L".sav");require(!std::filesystem::exists(sav),"legacy SRAM double write");
}
void ack(const std::filesystem::path& dir) {
    const auto request=fixture(dir);test::FakeAudioConsumer audio(false);EmulatorRunner runner(audio.stream);auto s=load(runner,request);
    runner.setPaused(false);s=waitFor(runner,[&](const auto& p){return p.queued==audio.stream.targetFrames();});
    runner.setMuted(true);runner.setInput({.button1=true},true);const auto before=fileBytes(request.databasePath);const auto serial=s->operationSerial;const auto time=s->tstates,queued=s->queued,sequence=audio.stream.control().sequence;
    HoldStop hold(audio);require(runner.updateRom(change(*s)),"update submit");awaitStop(audio,sequence);
    auto busy=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());require(busy->busy && busy->tstates==time && busy->ramBytes==8192 && audio.stream.pcm.queued()==queued && fileBytes(request.databasePath)==before,"changed before ack");
    require(!runner.updateRom(change(*s)) && !runner.load(request) && !runner.saveSram(),"busy request overwrite");
    runner.setPaused(true);hold.release();s=done(runner,serial);
    require(s->operationSucceeded && s->state==RunnerState::Paused && s->muted && s->ramBytes==16384 && s->tstates==0 && s->frames==0 && s->romChanged && s->frameChanged,"ack exchange");
    require(s->queued==0 && s->pending==0 && s->discardedQueued>=queued && s->romUpdate && s->romUpdate->converted==1 && !RunnerTestAccess::pressed(runner),"exchange accounting/input");
    const auto data=sram(request.databasePath);require(data.size()==16384 && data[0]==0x5a && data[8192]==0,"latest RAM update");
    require(runner.saveSram(),"save after update");s=done(runner,s->operationSerial);require(s->operationSucceeded && sram(request.databasePath)==data,"old RAM wrote back");
}
void failures(const std::filesystem::path& dir) {
    for(std::string_view fault:{"prepare","cheats","sql","commit","ack"}) {
        const auto request=fixture(dir);if(fault=="prepare" || fault=="sql") {
            auto db=raw(request.databasePath);sql(db.get(),fault=="prepare"?"UPDATE states SET data=x'00'":"CREATE TRIGGER fail BEFORE UPDATE ON rom_settings BEGIN SELECT RAISE(ABORT,'fixture');END");
        }
        test::FakeAudioConsumer audio(false);EmulatorRunner runner(audio.stream);auto old=load(runner,request);
        if(fault=="cheats") {
            auto db=raw(request.databasePath);
            sql(db.get(),"ALTER TABLE cheats RENAME TO unavailable_cheats");
        }
        runner.setPaused(false);old=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
        const auto before=fileBytes(request.databasePath);const auto sequence=audio.stream.control().sequence;HoldStop hold(audio);
        require(runner.updateRom(change(*old)),"failure submit");std::unique_ptr<RunnerSnapshot> s;
        if(fault=="prepare" || fault=="cheats") {s=done(runner,old->operationSerial);require(audio.stream.control().sequence==sequence && s->queued==old->queued && s->discardedQueued==old->discardedQueued && s->discardedPending==old->discardedPending,"prepare failure flushed audio");hold.release();}
        else {
            awaitStop(audio,sequence);runner.setPaused(true);auto reader=raw(request.databasePath);if(fault=="commit") sql(reader.get(),"BEGIN;SELECT * FROM states");
            if(fault=="ack") audio.stream.fail("fixture ack failure");hold.release();s=done(runner,old->operationSerial);if(fault=="commit") sql(reader.get(),"ROLLBACK");
        }
        require(!s->operationSucceeded && !s->error.empty() && s->ramBytes==8192 && s->tstates==old->tstates && s->databaseRecord==old->databaseRecord && fileBytes(request.databasePath)==before,"failed operation changed DB/System");
        if(fault=="cheats") require(s->error.find("no such table: cheats")!=std::string::npos,
            "cheat read failure reason was lost");
        if(fault=="sql" || fault=="commit") {
            if(fault=="sql") {auto db=raw(request.databasePath);sql(db.get(),"DROP TRIGGER fail");}
            require(runner.saveSram(),"save old System after commit failure");s=done(runner,s->operationSerial);require(s->operationSucceeded && sram(request.databasePath).size()==8192 && sram(request.databasePath)[0]==0x5a,"failure advanced baseline");
            require(runner.updateRom(change(*s)),"retry submit");s=done(runner,s->operationSerial);require(s->operationSucceeded && s->ramBytes==16384,"retry failed");
        }
    }
}
void stop(const std::filesystem::path& dir) {
    const auto request=fixture(dir);test::FakeAudioConsumer audio(false);EmulatorRunner runner(audio.stream);auto old=load(runner,request);
    runner.setPaused(false);old=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
    const auto before=fileBytes(request.databasePath);const auto sequence=audio.stream.control().sequence;HoldStop hold(audio);
    require(runner.updateRom(change(*old)),"stop update");awaitStop(audio,sequence);RunnerTestAccess::requestStop(runner);hold.release();runner.stop();
    const auto s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());require(s->state==RunnerState::Stopped && !s->busy && s->ramBytes==8192 && s->databaseRecord==old->databaseRecord && fileBytes(request.databasePath)==before,"stop committed/published candidate");
}
void metadata(const std::filesystem::path& dir) {
    const auto request=fixture(dir);test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);auto old=load(runner,request);old=dirty(runner);runner.setInput({.button1=true});
    const auto sequence=audio.stream.control().sequence,queued=old->discardedQueued;HoldStop hold(audio);RomUpdateRequest update;update.name=L"renamed";update.lastPath=dir/L"hint.sms";
    require(runner.updateRom(update),"metadata submit");auto s=done(runner,old->operationSerial);hold.release();
    require(s->operationSucceeded && !s->romChanged && !s->frameChanged && s->tstates==old->tstates && s->discardedQueued==queued && s->databaseRecord->name==L"renamed" && RunnerTestAccess::pressed(runner) && audio.stream.control().sequence==sequence,"metadata exchanged/flushed");
    require(sram(request.databasePath)[0]==0 && s->romPath==old->romPath,"metadata saved dirty SRAM/changed source");
    require(runner.saveSram(),"metadata subsequent save");s=done(runner,s->operationSerial);require(s->operationSucceeded && sram(request.databasePath)[0]==0x5a,"metadata changed baseline");
}
void fm(const std::filesystem::path& dir) {
    const auto request=fixture(dir,true);test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);auto old=load(runner,request);runner.setMuted(true);
    auto next=change(*old);next.settings->fmPlugin=L"acesms-fm-ymfm.dll";next.settings->interpolation=FmInterpolation::Linear;
    require(runner.updateRom(next),"FM update");auto s=done(runner,old->operationSerial);
    require(s->operationSucceeded && s->fmBackendName=="ymfm OPLL" && s->fmProfile->method==FmInterpolation::Linear && s->romUpdate->deleted==1 && s->muted && s->tstates==0,"FM swap");
    next.settings->fmPlugin=L"missing.dll";const auto sequence=audio.stream.control().sequence;require(runner.updateRom(next),"missing backend submit");old=done(runner,s->operationSerial);
    require(!old->operationSucceeded && old->fmBackendName==s->fmBackendName && old->ramBytes==s->ramBytes && audio.stream.control().sequence==sequence,"backend failure fallback/flush");
}
void running(const std::filesystem::path& dir) {
    const auto request=fixture(dir);test::FakeAudioConsumer audio(false);EmulatorRunner runner(audio.stream);auto s=load(runner,request);runner.setMuted(true);runner.setPaused(false);
    s=waitFor(runner,[&](const auto& p){return p.queued==audio.stream.targetFrames();});require(runner.updateRom(change(*s)),"running submit");s=done(runner,s->operationSerial);
    require(s->operationSucceeded && s->state==RunnerState::Running && s->muted && s->ramBytes==16384,"running flags");
    s=waitFor(runner,[&](const auto& p){return p.queued==audio.stream.targetFrames();});const auto time=s->tstates;audio.grant(480);waitFor(runner,[&](const auto& p){return p.tstates>time && p.queued==audio.stream.targetFrames();});
}
}
void stateSave(const std::filesystem::path& dir) {
    const auto request=fixture(dir);
    test::FakeAudioConsumer audio(false);
    EmulatorRunner runner(audio.stream);
    auto initial=load(runner,request);
    runner.setPaused(false);
    auto old=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
    const auto sequence=audio.stream.control().sequence;
    require(runner.saveDatabaseState(L"new"),"DB state save submit");
    auto saved=done(runner,old->operationSerial);
    require(saved->operationSucceeded && saved->savedStateId && *saved->savedStateId!=1,"new state ID");
    require(saved->tstates==old->tstates && saved->queued==old->queued &&
        audio.stream.control().sequence==sequence && sram(request.databasePath)[0]==0,"state save changed runtime or SRAM");
    const auto id=*saved->savedStateId;
    require(runner.saveDatabaseState(L"updated",id),"DB state overwrite submit");
    saved=done(runner,saved->operationSerial);
    require(saved->operationSucceeded && saved->savedStateId==id,"overwrite ID");
    auto opened=RomDatabase::open(request.databasePath);
    auto& db=std::get<RomDatabase>(opened);
    const StateProfile profile{32768,saved->romMetadata->crc32,Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,8192,48000,{}};
    auto stored=db.readState(saved->databaseRecord->id,id,profile);
    const auto& row=std::get<SavedState>(stored);
    require(row.name==L"updated" && row.state->tstates==saved->tstates && row.state->cartridge.ram[0]==0x5a,"stored snapshot");
    require(runner.saveDatabaseState(L"missing",999),"missing overwrite submit");
    saved=done(runner,saved->operationSerial);
    require(!saved->operationSucceeded && !saved->savedStateId,"failed overwrite published ID");
    require(!db.execute("UPDATE states SET name=char(0) WHERE state_id=1;"
        "CREATE TRIGGER reject_slot BEFORE INSERT ON states BEGIN SELECT RAISE(ABORT,'slot write refused');END"),
        "state save and catalog error fixture");
    const auto before=fileBytes(request.databasePath);
    require(runner.saveDatabaseSlot(5),"slot save with unavailable catalog submit");
    saved=done(runner,saved->operationSerial);
    require(!saved->operationSucceeded && !saved->savedStateId && !saved->databaseStates &&
        saved->error.find("slot write refused")!=std::string::npos,"state catalog failure masked save failure");
    require(saved->tstates==old->tstates && fileBytes(request.databasePath)==before,
        "failed state save changed the session or DB");
    require(!db.execute("DROP TRIGGER reject_slot"),"remove slot write failure");
    require(runner.saveDatabaseSlot(5),"save with catalog-only failure submit");
    saved=done(runner,saved->operationSerial);
    require(!saved->operationSucceeded && saved->savedStateId && !saved->databaseStates &&
        saved->error=="Invalid state name","catalog-only failure was not reported");
    require(std::holds_alternative<SavedState>(db.readState(saved->databaseRecord->id,*saved->savedStateId,profile)),
        "catalog-only failure undid the successful save");
}
void stateLoad(const std::filesystem::path& dir) {
    for(std::string_view result:{"success","ack_failure","stop"}) {
        const auto request=fixture(dir,true);
        test::FakeAudioConsumer audio(false);
        EmulatorRunner runner(audio.stream);
        auto old=load(runner,request);
        runner.setMuted(true);runner.setPaused(false);
        old=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
        runner.setInput({.button1=true},true);
        const auto before=fileBytes(request.databasePath);
        const auto sequence=audio.stream.control().sequence;
        HoldStop hold(audio);
        require(runner.loadDatabaseState(1),"DB state load submit");
        awaitStop(audio,sequence);
        auto busy=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
        require(busy->busy && busy->tstates==old->tstates && !busy->frameChanged &&
            audio.stream.pcm.queued()==old->queued && RunnerTestAccess::pressed(runner),"exchanged before ack");
        require(!runner.saveDatabaseState(L"busy") && !runner.loadDatabaseState(1),"DB state busy");
        require(!runner.listDatabaseStates() && !runner.deleteDatabaseState(1),"DB state catalog busy");
        runner.setPaused(true);
        if(result=="ack_failure") audio.stream.fail("fixture ack failure");
        if(result=="stop") RunnerTestAccess::requestStop(runner);
        hold.release();
        if(result=="stop") runner.stop();
        auto restored=result=="stop"?std::make_unique<RunnerSnapshot>(runner.takeSnapshot()):done(runner,old->operationSerial);
        require(fileBytes(request.databasePath)==before,"state load changed database");
        if(result!="success") {
            require(!restored->operationSucceeded && restored->tstates==old->tstates,"failed/stopped load exchanged System");
            continue;
        }
        require(restored->operationSucceeded && restored->state==RunnerState::Paused && restored->muted &&
            restored->tstates==0 && restored->frames==0 && restored->frameChanged && !restored->romChanged &&
            restored->queued==0 && restored->pending==0 && !RunnerTestAccess::pressed(runner),"DB state exchange");
        require(runner.saveSram(),"save restored RAM");
        restored=done(runner,restored->operationSerial);
        require(restored->operationSucceeded && sram(request.databasePath)[0]==0x44,"restored RAM not saved");
    }
}
void stateReject(const std::filesystem::path& dir) {
    const auto request=fixture(dir);
    test::FakeAudioConsumer audio(false);
    EmulatorRunner runner(audio.stream);
    require(!runner.saveDatabaseState(L"idle") && !runner.loadDatabaseState(1),"idle accepted DB state");
    auto old=load(runner,request);
    runner.setPaused(false);
    old=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
    const auto sequence=audio.stream.control().sequence;
    for(const auto id:{999,1}) {
        if(id==1) {
            auto db=raw(request.databasePath);
            sql(db.get(),"UPDATE states SET data=x'00'");
        }
        require(runner.loadDatabaseState(id),"rejection submit");
        auto failed=done(runner,old->operationSerial);
        require(!failed->operationSucceeded && !failed->error.empty() && failed->tstates==old->tstates &&
            failed->queued==old->queued && audio.stream.control().sequence==sequence,"bad state stopped consumer");
        old=std::move(failed);
    }
}
void stateCatalog(const std::filesystem::path& dir) {
    const auto request=fixture(dir);
    auto other=raw(request.databasePath);
    sql(other.get(),"INSERT INTO roms VALUES(2,zeroblob(32),zeroblob(20),0,32768,'other','other.sms');"
        "INSERT INTO states(state_id,rom_id,name,created_at,updated_at,data) VALUES(2,2,'other',1,2,x'00'),(3,1,'state',3,4,x'00');"
        "UPDATE states SET created_at=5,updated_at=6 WHERE state_id=1");
    test::FakeAudioConsumer audio(false);
    EmulatorRunner runner(audio.stream);
    require(!runner.listDatabaseStates() && !runner.deleteDatabaseState(1),"idle accepted state catalog");
    load(runner,request);
    runner.setPaused(false);
    auto snapshot=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
    const auto time=snapshot->tstates;
    const auto queued=snapshot->queued;
    const auto sequence=audio.stream.control().sequence;
    runner.setInput({.button1=true});
    const auto list=[&] {
        require(runner.listDatabaseStates(),"state list submit");
        snapshot=done(runner,snapshot->operationSerial);
        require(snapshot->operation==RunnerOperation::ListStates && snapshot->operationSucceeded &&
            snapshot->databaseStates,"state list result");
        return *snapshot->databaseStates;
    };
    const auto erase=[&](int64_t id,bool succeeds) {
        require(runner.deleteDatabaseState(id),"state delete submit");
        snapshot=done(runner,snapshot->operationSerial);
        require(snapshot->operation==RunnerOperation::DeleteState && snapshot->operationSucceeded==succeeds &&
            snapshot->error.empty()==succeeds,"state delete result");
    };
    const auto rows=list();
    require(rows.size()==2 && rows[0].id==1 && rows[1].id==3 && rows[0].name==L"state" &&
        rows[1].name==rows[0].name && rows[0].createdAt==5 && rows[0].updatedAt==6 &&
        rows[1].createdAt==3 && rows[1].updatedAt==4,"state list scope, order or metadata");
    sql(other.get(),"UPDATE states SET updated_at=-1 WHERE state_id=3");
    require(runner.listDatabaseStates(),"invalid metadata list submit");
    snapshot=done(runner,snapshot->operationSerial);
    require(!snapshot->operationSucceeded && !snapshot->error.empty() &&
        !snapshot->databaseStates,"list returned partial or stale results");
    sql(other.get(),"UPDATE states SET updated_at=4 WHERE state_id=3");
    const auto before=fileBytes(request.databasePath);
    erase(2,false);
    erase(999,false);
    sql(other.get(),"BEGIN;SELECT * FROM states");
    erase(3,false);
    sql(other.get(),"ROLLBACK");
    require(fileBytes(request.databasePath)==before,"failed deletion changed DB");
    erase(3,true);
    auto remaining=list();
    require(remaining.size()==1 && remaining[0].id==1,"delete affected unintended state");
    erase(3,false);
    erase(1,true);
    require(list().empty(),"empty list failed");
    require(snapshot->tstates==time && snapshot->queued==queued && RunnerTestAccess::pressed(runner) &&
        audio.stream.control().sequence==sequence && sram(request.databasePath)[0]==0,"catalog altered runtime or SRAM");
    sql(other.get(),"CREATE TEMP TABLE preserved(ok INTEGER CHECK(ok=1));"
        "INSERT INTO preserved SELECT count(*) FROM states WHERE rom_id=2 AND state_id=2 AND data=x'00'");
}
void registration(const std::filesystem::path& dir) {
    const auto active=fixture(dir);
    auto request=active;
    request.databasePath=dir/L"registration.sqlite3";
    std::filesystem::remove(request.databasePath);
    test::FakeAudioConsumer audio(false);
    EmulatorRunner runner(audio.stream);
    auto snapshot=load(runner,active);
    runner.setPaused(false);
    snapshot=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
    const auto time=snapshot->tstates;
    const auto sequence=audio.stream.control().sequence;
    const auto submit=[&] {
        require(runner.load(request),"registration request");
        snapshot=done(runner,snapshot->operationSerial);
    };
    submit();
    require(!snapshot->operationSucceeded && snapshot->unregisteredRom && snapshot->error.empty() &&
        snapshot->databaseRecord->name==L"old" && snapshot->tstates==time &&
        audio.stream.control().sequence==sequence,"unknown ROM disturbed old session");
    auto identity=snapshot->unregisteredRom->identity;
    auto inspect=RomDatabase::open(request.databasePath);
    auto& db=std::get<RomDatabase>(inspect);
    const auto absent=[&] {return !std::get<std::optional<RomRecord>>(db.findRom(identity));};
    require(absent(),"probe registered ROM");
    RomSettings settings;
    settings.ramBytes=16384;
    request.registration=RomRegistration{identity,settings,L"registered"};
    request.registration->identity.sha256[0]^=1;
    submit();
    require(!snapshot->operationSucceeded && !snapshot->unregisteredRom && !snapshot->error.empty() && absent(),"different identity registered");
    request.registration->identity=identity;
    request.registration->settings.ramBytes=123;
    submit();
    require(!snapshot->operationSucceeded && absent() && audio.stream.control().sequence==sequence,"invalid settings reached registration");
    request.registration->settings=settings;
    request.registration->settings.fmEnabled=true;
    request.registration->settings.fmPlugin=L"missing.dll";
    submit();
    require(!snapshot->operationSucceeded && absent() && audio.stream.control().sequence==sequence,"missing FM backend registered");
    request.registration->settings=settings;
    auto contents=fileBytes(request.path);
    {
        std::fstream changed(request.path,std::ios::binary|std::ios::in|std::ios::out);
        changed.put(static_cast<char>(contents.front()^1));
        require(bool(changed),"fixture replacement");
    }
    submit();
    require(!snapshot->operationSucceeded && !snapshot->unregisteredRom && absent() &&
        audio.stream.control().sequence==sequence,"changed ROM registered");
    {
        std::ofstream original(request.path,std::ios::binary|std::ios::trunc);
        original.write(contents.data(),contents.size());
        require(bool(original),"fixture restoration");
    }
    {
        auto connection=raw(request.databasePath);
        sql(connection.get(),"CREATE TRIGGER fail BEFORE INSERT ON rom_settings BEGIN SELECT RAISE(ABORT,'fixture');END");
        submit();
        require(!snapshot->operationSucceeded && snapshot->databaseRecord->name==L"old" && absent(),"failed SQL exchanged or registered");
        sql(connection.get(),"DROP TRIGGER fail");
    }
    snapshot=waitFor(runner,[&](const auto& s){return s.queued==audio.stream.targetFrames();});
    const auto oldTime=snapshot->tstates;
    const auto oldSequence=audio.stream.control().sequence;
    HoldStop hold(audio);
    require(runner.load(request),"confirmed registration submit");
    awaitStop(audio,oldSequence);
    require(absent() && std::make_unique<RunnerSnapshot>(runner.takeSnapshot())->tstates==oldTime,"registered/exchanged before ack");
    runner.setPaused(true);
    hold.release();
    snapshot=done(runner,snapshot->operationSerial);
    require(snapshot->operationSucceeded && snapshot->databaseRecord->name==L"registered" && snapshot->ramBytes==16384 &&
        snapshot->tstates==0 && sram(request.databasePath)==std::vector<uint8_t>(16384),"registration did not start from DB settings");
    const auto id=snapshot->databaseRecord->id;
    runner.stop();

    test::FakeAudioConsumer nextAudio(false);
    EmulatorRunner next(nextAudio.stream);
    next.setPaused(true);
    request.registration.reset();
    request.path=dir/L"renamed.gg";
    std::filesystem::copy_file(active.path,request.path,std::filesystem::copy_options::overwrite_existing);
    request.machine=Machine::GameGear;
    request.standard=VideoStandard::Pal;
    require(next.load(request),"reopen registered");
    snapshot=done(next,0);
    require(snapshot->operationSucceeded && snapshot->databaseRecord->id==id && snapshot->machine==Machine::Sms &&
        snapshot->ramBytes==16384 && snapshot->databaseRecord->settings==settings,"did not reuse saved settings");

    // A failed consumer stop must leave a fresh database unregistered.
    request.databasePath=dir/L"registration-ack.sqlite3";
    std::filesystem::remove(request.databasePath);
    request.registration=RomRegistration{identity,settings,L"ack failure"};
    next.setPaused(false);
    snapshot=waitFor(next,[&](const auto& s){return s.queued==nextAudio.stream.targetFrames();});
    const auto ackSequence=nextAudio.stream.control().sequence;
    HoldStop failAck(nextAudio);
    require(next.load(request),"ack-failure registration submit");
    awaitStop(nextAudio,ackSequence);
    nextAudio.stream.fail("fixture registration ack failure");
    failAck.release();
    const auto failed=done(next,snapshot->operationSerial);
    auto failedOpen=RomDatabase::open(request.databasePath);
    require(!failed->operationSucceeded && failed->tstates==snapshot->tstates &&
        !std::get<std::optional<RomRecord>>(std::get<RomDatabase>(failedOpen).findRom(identity)),"failed ack registered or exchanged");
}
void databasePath(const std::filesystem::path& dir) {
    std::array<wchar_t,32768> module{};
    const auto length=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    require(length && length<module.size(),"test executable path");
    const auto expected=std::filesystem::path(module.data()).parent_path()/L"acesms.sqlite3";
    const auto workingDirectory=std::filesystem::current_path();
    std::filesystem::current_path(dir);
    const auto resolved=resolveDatabasePath({});
    std::filesystem::current_path(workingDirectory);
    const auto* standard=std::get_if<std::filesystem::path>(&resolved);
    require(standard && *standard==expected && standard->parent_path()!=dir,"standard DB must be beside the executable");

    auto request=fixture(dir);
    test::FakeAudioConsumer audio;
    EmulatorRunner runner(audio.stream);
    auto snapshot=load(runner,request);
    const auto previous=*snapshot->databaseRecord;
    const auto parent=dir/L"first use";
    request.databasePath=parent/L"acesms.sqlite3";
    request.useDefaultDatabase=true; // Explicit fixture must take precedence.
    std::filesystem::remove(request.databasePath);
    std::filesystem::remove(parent);
    require(runner.load(request),"first DB path submit");
    snapshot=done(runner,snapshot->operationSerial);
    require(snapshot->unregisteredRom && snapshot->error.empty() && snapshot->databaseRecord==previous &&
        std::filesystem::is_regular_file(request.databasePath),"first DB path not initialized");
    const auto identity=snapshot->unregisteredRom->identity;
    request.registration=RomRegistration{identity,previous.settings,L"first use"};
    require(runner.load(request),"register in new directory");
    snapshot=done(runner,snapshot->operationSerial);
    require(snapshot->operationSucceeded && snapshot->databaseRecord->name==L"first use","registration in new directory");

    request.registration.reset();
    request.databasePath=request.path/L"unreachable.sqlite3";
    const auto record=snapshot->databaseRecord;
    const auto sequence=audio.stream.control().sequence;
    require(runner.load(request),"blocked DB path submit");
    snapshot=done(runner,snapshot->operationSerial);
    require(!snapshot->operationSucceeded && !snapshot->unregisteredRom &&
        snapshot->error.find("create database directory")!=std::string::npos &&
        snapshot->databaseRecord==record && audio.stream.control().sequence==sequence,"blocked directory replaced session or fell back");

    const auto activePath=snapshot->databasePath;
    request.databasePath=dir/L"read only \u65e5.sqlite3";
    std::filesystem::copy_file(activePath,request.databasePath,std::filesystem::copy_options::overwrite_existing);
    const auto before=fileBytes(request.databasePath);
    require(SetFileAttributesW(request.databasePath.c_str(),FILE_ATTRIBUTE_READONLY),"set read-only fixture");
    struct RestoreAttributes {
        const std::filesystem::path& path;
        ~RestoreAttributes() {SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);}
    } restore{request.databasePath};
    require(runner.load(request),"read-only DB submit");
    snapshot=done(runner,snapshot->operationSerial);
    const auto utf8=request.databasePath.u8string();
    require(!snapshot->operationSucceeded && !snapshot->unregisteredRom &&
        snapshot->error.find(std::string(utf8.begin(),utf8.end()))!=std::string::npos &&
        (snapshot->error.find("write access")!=std::string::npos || snapshot->error.find("readonly")!=std::string::npos),
        "read-only DB error must include path and reason");
    require(snapshot->databaseRecord==record && snapshot->databasePath==activePath &&
        audio.stream.control().sequence==sequence && fileBytes(request.databasePath)==before,"read-only load changed session or database");
}
void sg1000(const std::filesystem::path& dir) {
    for (const size_t size : {40960, 8192}) {
        LoadRequest request;
        request.path = dir / (std::to_wstring(size) + L".sg");
        request.databasePath = dir / (std::to_wstring(size) + L".sqlite3");
        request.machine.reset();
        std::filesystem::remove(request.databasePath);
        auto bytes = test::sg1000TestRom();
        bytes.resize(size);
        {
            std::ofstream file(request.path, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            require(bool(file), "SG-1000 fixture write");
        }
        int64_t stateId{};
        uint64_t savedTime{};
        {
            test::FakeAudioConsumer audio;
            EmulatorRunner runner(audio.stream);
            runner.setPaused(true);
            require(runner.load(request), "SG-1000 lookup");
            auto s = done(runner, 0);
            require(s->unregisteredRom && s->unregisteredRom->identity.bytes == size, "SG-1000 identity lookup");
            RomSettings settings;
            settings.machine = Machine::Sg1000;
            settings.mapper = MapperKind::RomOnly;
            settings.ramBytes = 0;
            request.registration = RomRegistration{s->unregisteredRom->identity, settings, L"diagnostic"};
            require(runner.load(request), "SG-1000 registration");
            s = done(runner, s->operationSerial);
            require(s->operationSucceeded && s->machine == Machine::Sg1000 && s->databaseRecord
                && !s->fmEnabled && !s->sramEnabled, "SG-1000 registration/startup");
            runner.setInput({},false,{.left=true,.button2=true});
            s = dirty(runner);
            require(s->pixels[255].g == 200, "SG-1000 frame");
            require(runner.saveDatabaseState(L"SG-1000 save"), "SG-1000 save request");
            s = done(runner, s->operationSerial);
            require(s->operationSucceeded && s->savedStateId, "SG-1000 save");
            stateId = *s->savedStateId;
            savedTime = s->tstates;
            const auto& record=*s->databaseRecord;
            const StateProfile profile{static_cast<uint32_t>(size),record.identity.crc32,Machine::Sg1000,
                VideoStandard::Ntsc,MapperKind::RomOnly,0,48000,{}};
            auto opened=RomDatabase::open(request.databasePath);
            auto saved=std::get<RomDatabase>(opened).readState(record.id,stateId,profile);
            const auto* checkpoint=std::get_if<SavedState>(&saved);
            require(checkpoint && digitalPadBits(checkpoint->state->controller.player2)==0x24,"SG-1000 worker 2P state");
            require(runner.prepareClose() && done(runner, s->operationSerial)->closeReady, "SG-1000 close");
            runner.stop();
        }
        request.registration.reset();
        request.machine = Machine::GameGear;
        test::FakeAudioConsumer audio;
        EmulatorRunner runner(audio.stream);
        runner.setPaused(true);
        require(runner.load(request), "SG-1000 reload");
        auto s = done(runner, 0);
        require(s->operationSucceeded && s->machine == Machine::Sg1000 && s->tstates == 0,
            "SG-1000 saved settings");
        require(runner.loadDatabaseState(stateId), "SG-1000 restore request");
        s = done(runner, s->operationSerial);
        require(s->operationSucceeded && s->tstates == savedTime && s->pixels[255].g == 200,
            "SG-1000 restored frame/time");
        if (size == 8192) {
            auto update = change(*s, 0);
            update.settings->machine = Machine::Sms;
            update.preview = true;
            require(runner.updateRom(update), "SG-1000 edit review");
            s = done(runner, s->operationSerial);
            require(s->operationSucceeded && s->romUpdateImpact && s->romUpdateImpact->states.size() == 1
                && s->machine == Machine::Sg1000 && s->tstates == savedTime, "SG-1000 review changed session");
            update.preview = false;
            update.confirmedImpact = s->romUpdateImpact;
            require(runner.updateRom(update), "SG-1000 confirmed edit");
            s = done(runner, s->operationSerial);
            require(s->operationSucceeded && s->romUpdate && s->romUpdate->deleted == 1
                && s->machine == Machine::Sms && s->tstates == 0, "SG-1000 incompatible state deletion");
        }
    }
}

void zipRom(const std::filesystem::path& dir) {
    auto request=fixture(dir);
    const auto payload=fileBytes(request.path);
    auto alternate=payload;
    alternate.back()^=1;
    const auto prefixed=std::string(512,'p')+payload;
    const auto archive=dir/L"bundle \u65e5\u672c\u8a9e.ZIP";
    test::writeZip(archive,{{"notes.txt","ROM fixture"},{"dir.sms/",""},
        {"folder/z-first.GG",prefixed},{"a-next.sms",alternate,MZ_NO_COMPRESSION}});

    test::FakeAudioConsumer audio;
    EmulatorRunner runner(audio.stream);
    auto s=load(runner,request);
    const auto record=*s->databaseRecord;
    s=dirty(runner);
    request.path=archive;
    request.machine=Machine::GameGear;
    require(runner.load(request),"ZIP load submit");
    s=done(runner,s->operationSerial);
    require(s->operationSucceeded && s->romPath==archive && s->databaseRecord==record &&
        s->machine==Machine::Sms && s->sramEnabled,"ZIP did not use first ROM and existing settings");
    require(sram(request.databasePath)[0]==0x5a,"ROM switch did not retain DB SRAM");

    auto opened=RomDatabase::open(request.databasePath);
    auto& db=std::get<RomDatabase>(opened);
    const StateProfile profile{32768,record.identity.crc32,Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,8192,48000,{}};
    const auto captureRam=[&](uint8_t expected) {
        require(runner.saveDatabaseState(L"probe"),"capture ZIP session");
        s=done(runner,s->operationSerial);
        require(s->operationSucceeded && s->savedStateId,"ZIP state save");
        auto state=db.readState(record.id,*s->savedStateId,profile);
        require(std::get<SavedState>(state).state->cartridge.ram[0]==expected,"ZIP session SRAM value");
    };
    captureRam(0x5a);
    require(runner.loadDatabaseState(1),"restore raw ROM state in ZIP session");
    s=done(runner,s->operationSerial);
    require(s->operationSucceeded,"raw ROM state rejected in ZIP session");
    captureRam(0x44);

    auto fail=[&](const std::filesystem::path& path,std::string_view reason) {
        request.path=path;
        require(runner.load(request),"bad ZIP submit");
        s=done(runner,s->operationSerial);
        if(s->error.find(reason)==std::string::npos) std::cerr<<s->error<<'\n';
        require(!s->operationSucceeded && s->romPath==archive && s->databaseRecord==record &&
            s->error.find(reason)!=std::string::npos,"bad ZIP changed session or lacked diagnostic");
    };
    const auto invalid=dir/"invalid.zip";
    test::writeZip(invalid,{{"bad.sg","short"},{"valid.sms",payload}});
    fail(invalid,"bad.sg");
    test::writeZip(invalid,{{"notes.txt","no ROM"}});
    fail(invalid,"No supported ROM");
    test::writeZip(invalid,{{"oversized.gg",std::string(1048576+513,'x')},{"valid.sms",payload}});
    fail(invalid,"ROM size exceeds");
    test::writeZip(invalid,{{"broken.sms",payload,MZ_NO_COMPRESSION},{"valid.sms",payload}});
    const auto offset=fileBytes(invalid).find(payload);
    require(offset!=std::string::npos,"stored ZIP payload");
    {
        std::fstream file(invalid,std::ios::binary|std::ios::in|std::ios::out);
        file.seekp(offset);
        file.put(static_cast<char>(payload[0]^1));
        require(bool(file),"ZIP corruption fixture");
    }
    fail(invalid,"broken.sms: CRC-32 check failed");

    const auto fresh=dir/"fresh.zip";
    test::writeZip(fresh,{{"folder/\xe6\x97\xa5-new.sms",alternate}});
    request.path=fresh;
    require(runner.load(request),"unregistered ZIP lookup");
    s=done(runner,s->operationSerial);
    require(s->unregisteredRom && s->unregisteredRom->archiveEntry==L"folder/\u65e5-new.sms" &&
        s->databaseRecord==record,"unregistered ZIP lookup changed session");
    request.registration=RomRegistration{s->unregisteredRom->identity,record.settings,L"new"};
    require(runner.load(request),"ZIP registration");
    s=done(runner,s->operationSerial);
    require(s->operationSucceeded && s->databaseRecord && s->databaseRecord->id!=record.id &&
        s->databaseRecord->lastPath==fresh && s->archiveEntry==L"folder/\u65e5-new.sms","ZIP registration metadata");
}

void cheats(const std::filesystem::path& dir) {
    const auto request=fixture(dir);
    test::FakeAudioConsumer audio;
    EmulatorRunner runner(audio.stream);
    auto s=load(runner,request);
    const auto capture=[&] {
        require(runner.captureCheats(),"capture submit");
        s=done(runner,s->operationSerial);
        require(s->operationSucceeded && s->state==RunnerState::Paused && s->cheatSnapshot,"capture result");
    };
    capture();
    require(s->cheatSnapshot->wasPaused && s->cheatSnapshot->definitions.empty() && s->cheatSnapshot->ram.size()==8192,"initial capture");
    const Cheat once{L"Once",0xc090,SearchWidth::Word,0x1234,true,false};
    require(runner.applyCheat(once),"apply submit");
    s=done(runner,s->operationSerial);
    require(s->operationSucceeded && s->cheatSnapshot && s->tstates==0 && s->cheatSnapshot->ram[0x90]==0x34 &&
        s->cheatSnapshot->ram[0x91]==0x12,"single write while paused");
    const Cheat freeze{L"Freeze",0xc092,SearchWidth::Byte,0x55,true,true};
    require(runner.saveCheats({freeze}),"save submit");
    s=done(runner,s->operationSerial);require(s->operationSucceeded,"save definitions");
    require(runner.saveCheats({freeze,freeze}),"invalid save submit");
    s=done(runner,s->operationSerial);require(!s->operationSucceeded,"invalid save accepted");
    capture();
    require(s->cheatSnapshot->definitions==std::vector{freeze} && s->cheatSnapshot->ram[0x92]==0,"save changed paused RAM or active list");
    runner.setPaused(false);
    s=waitFor(runner,[](const auto& p){return p.frames>=1;});
    capture();
    require(!s->cheatSnapshot->wasPaused && s->cheatSnapshot->ram[0x92]==0x55,"normal frame freeze");
    require(runner.loadDatabaseState(1),"state restore submit");
    s=done(runner,s->operationSerial);require(s->operationSucceeded && s->tstates==0,"restore v1");
    capture();
    require(s->cheatSnapshot->ram[0x90]==0 && s->cheatSnapshot->ram[0x92]==0 &&
        s->cheatSnapshot->definitions==std::vector{freeze},"restore changed definitions or applied early");
    runner.setFastForward(true);
    runner.setPaused(false);
    s=waitFor(runner,[](const auto& p){return p.fastForward && p.frames>=1;});
    capture();
    require(s->cheatSnapshot->ram[0x92]==0x55,"fast frame freeze");
    require(runner.load(request),"reload submit");
    s=done(runner,s->operationSerial);require(s->operationSucceeded,"reload");
    capture();
    require(s->cheatSnapshot->ram[0x92]==0 && s->cheatSnapshot->definitions==std::vector{freeze},"reload definitions");
}

int main(int argc,char** argv) {
    try {require(argc==3,"arguments");const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);const std::string_view op=argv[1];
        if(op=="state_save") stateSave(dir);
        else if(op=="state_load") stateLoad(dir);
        else if(op=="state_reject") stateReject(dir);
        else if(op=="state_catalog") stateCatalog(dir);
        else if(op=="registration") registration(dir);
        else if(op=="database_path") databasePath(dir);
        else if(op=="sg1000") sg1000(dir);
        else if(op=="zip") zipRom(dir);
        else if(op=="cheats") cheats(dir);
        else if(op=="load") loading(dir);else if(op=="ack") {ack(dir);commitResponsiveness(dir/"commit");}else if(op=="failures") failures(dir);else if(op=="stop") stop(dir);else if(op=="metadata") metadata(dir);else if(op=="fm") fm(dir);else if(op=="running") running(dir);else require(false,"operation");return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
