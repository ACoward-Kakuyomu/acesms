#include "win32/DbSramStore.h"
#include "win32/EmulationSession.h"
#include "win32/FmPlugin.h"
#include "core/cartridge/RomLoader.h"
#include "../system/Stage8TestRom.h"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace smsgg;
using namespace smsgg::win32;
void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
RomImage image(MapperKind mapper=MapperKind::Sega,uint32_t ram=8192,uint8_t tag=0) {
    auto bytes=test::stage8FmRom();bytes.resize(32768);bytes.back()=tag;
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{mapper,ram}));
}
RomDatabase database(const std::filesystem::path& dir) {
    const auto path=dir/L"update.sqlite3";std::filesystem::remove(path);auto opened=RomDatabase::open(path);
    require(std::holds_alternative<RomDatabase>(opened),"open");return std::get<RomDatabase>(std::move(opened));
}
RomSettings settings(uint32_t ram=8192) {RomSettings s;s.ramBytes=ram;return s;}
RomRecord add(RomDatabase& db,const RomImage& rom,const RomSettings& s) {
    auto result=db.registerRom(std::get<RomIdentity>(identifyRom(rom)),s,L"old",L"old.sms");require(std::holds_alternative<RomRecord>(result),"register");return std::get<RomRecord>(std::move(result));
}
std::unique_ptr<EmulatorSystemState> capture(const EmulatorSystem& sys) {auto s=std::make_unique<EmulatorSystemState>();sys.captureState(*s);return s;}
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement statement(RomDatabase& db,const char* sql) {sqlite3_stmt* p=nullptr;require(sqlite3_prepare_v2(db.nativeHandle(),sql,-1,&p,nullptr)==SQLITE_OK,"prepare");return Statement(p,sqlite3_finalize);}
struct Checkpoint {int64_t id;StateProfile profile;std::vector<uint8_t> bytes;};
Checkpoint checkpoint(RomDatabase& db,int64_t romId,const RomSettings& s,const FmBackendPtr& backend={},uint32_t rate=44100,uint8_t tag=0) {
    auto rom=image(s.mapper,s.ramBytes,tag);EmulationSession session(rom,{},s.video,rate,s.machine,s.fmEnabled,s.interpolation,backend);
    for(unsigned n=0;n<1000;++n) require(std::holds_alternative<uint32_t>(session.system().step()),"step");
    auto state=capture(session.system());std::fill(state->cartridge.ram.begin(),state->cartridge.ram.end(),0x51);
    const auto bytes=encodeState(session.profile(),*state);
    // Synthetic historical rows can have different saved profiles under the same ROM.
    auto p=statement(db,"INSERT INTO states(rom_id,name,created_at,updated_at,data) VALUES(?1,'checkpoint',123,456,?2)");
    require(sqlite3_bind_int64(p.get(),1,romId)==SQLITE_OK && sqlite3_bind_blob(p.get(),2,bytes.data(),static_cast<int>(bytes.size()),SQLITE_STATIC)==SQLITE_OK && sqlite3_step(p.get())==SQLITE_DONE,"seed checkpoint");
    return {sqlite3_last_insert_rowid(db.nativeHandle()),session.profile(),bytes};
}
void seed(RomDatabase& db,const Checkpoint& cp,std::span<const uint8_t> bytes) {
    auto p=statement(db,"UPDATE states SET data=?1 WHERE state_id=?2");require(sqlite3_bind_blob(p.get(),1,bytes.data(),static_cast<int>(bytes.size()),SQLITE_STATIC)==SQLITE_OK && sqlite3_bind_int64(p.get(),2,cp.id)==SQLITE_OK && sqlite3_step(p.get())==SQLITE_DONE,"seed data");
}
SavedState read(RomDatabase& db,int64_t romId,int64_t id,const StateProfile& p) {auto value=db.readState(romId,id,p);require(std::holds_alternative<SavedState>(value),"read");return std::get<SavedState>(std::move(value));}
std::string fileBytes(const std::filesystem::path& dir) {std::ifstream f(dir/L"update.sqlite3",std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
struct Trace {
    sqlite3* db;int count{};static int call(unsigned,void* p,void*,void*) {++static_cast<Trace*>(p)->count;return 0;}
    explicit Trace(RomDatabase& d):db(d.nativeHandle()) {sqlite3_trace_v2(db,SQLITE_TRACE_STMT,call,this);}
    ~Trace(){sqlite3_trace_v2(db,0,nullptr,nullptr);}
};
RomUpdateCounts update(DbSramStore& store,RomRecord& record,std::vector<uint8_t>& ram,const RomSettings& next,const RomUpdateContext* ctx=nullptr) {
    auto value=store.updateRom(record,ram,next,L"\u65b0 ' ; --",L"new.sms",ctx);
    if(auto* error=std::get_if<DatabaseError>(&value)) throw std::runtime_error(error->message);
    return std::get<RomUpdateCounts>(value);
}
void metadata(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=image();auto record=add(db,rom,settings());DbSramStore store(db,record);auto ram=std::get<std::vector<uint8_t>>(store.load());const auto zero=ram;ram[0]=0xa7;
    const auto cp=checkpoint(db,record.id,settings());seed(db,cp,std::array<uint8_t,1>{0});const auto old=record;
    const auto counts=update(store,record,ram,record.settings);require(counts.kept==1 && !counts.converted && !counts.deleted && ram[0]==0xa7,"metadata preserves RAM/state");
    {Trace trace(db);require(!store.save(zero) && !trace.count,"metadata changed baseline");}
    auto found=db.findRom(record.identity);require(std::get<std::optional<RomRecord>>(found)==record && record.name==L"\u65b0 ' ; --","metadata persisted");
    auto stale=old;const auto before=fileBytes(dir);require(std::holds_alternative<DatabaseError>(store.updateRom(stale,ram,record.settings,L"stale",{},nullptr)) && fileBytes(dir)==before,"stale record accepted");
    auto invalid=record.settings;invalid.ramBytes=0xffffffffu;require(std::holds_alternative<DatabaseError>(store.updateRom(record,ram,invalid,L"bad",{},nullptr)) && fileBytes(dir)==before,"invalid setting allocated/applied");
}
void mixed(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=image();auto record=add(db,rom,settings());DbSramStore store(db,record);auto ram=std::get<std::vector<uint8_t>>(store.load());std::fill(ram.begin(),ram.end(),0xa7);
    auto next=settings(16384);const auto keep=checkpoint(db,record.id,next,{},96000),convert=checkpoint(db,record.id,settings());
    auto pal=settings();pal.video=VideoStandard::Pal;const auto remove=checkpoint(db,record.id,pal);
    auto cm=settings();cm.mapper=MapperKind::Codemasters;const auto remove2=checkpoint(db,record.id,cm);
    const auto otherRom=image(MapperKind::Sega,8192,1);const auto other=add(db,otherRom,settings());const auto otherCp=checkpoint(db,other.id,settings(),{},44100,1);
    const RomUpdateContext ctx{rom,48000,{}, {}};const auto counts=update(store,record,ram,next,&ctx);
    require(counts.kept==1 && counts.converted==1 && counts.deleted==2,"mixed actions");
    require(ram.size()==16384 && ram[0]==0xa7 && ram[8192]==0 && record.settings==next,"latest SRAM/caller settings");
    require(encodeState(keep.profile,*read(db,record.id,keep.id,keep.profile).state)==keep.bytes,"keep bytes");
    auto p=convert.profile;p.ramBytes=16384;const auto converted=read(db,record.id,convert.id,p);auto expected=decodeState(convert.bytes,convert.profile);
    auto& value=*std::get<std::unique_ptr<EmulatorSystemState>>(expected);value.cartridge.ram.resize(16384,0);
    require(encodeState(p,*converted.state)==encodeState(p,value) && converted.createdAt==123 && converted.updatedAt==456 && converted.name==L"checkpoint","convert values/metadata");
    require(std::holds_alternative<DatabaseError>(db.readState(record.id,remove.id,remove.profile)) && std::holds_alternative<DatabaseError>(db.readState(record.id,remove2.id,remove2.profile)),"incompatible retained");
    require(encodeState(otherCp.profile,*read(db,other.id,otherCp.id,otherCp.profile).state)==otherCp.bytes,"other ROM changed");
    {Trace trace(db);require(!store.save(ram) && !trace.count,"new baseline");}
    auto reopened=RomDatabase::open(dir/L"update.sqlite3");require(std::holds_alternative<RomDatabase>(reopened),"reopen");DbSramStore again(std::get<RomDatabase>(reopened),record);require(std::get<std::vector<uint8_t>>(again.load())==ram,"reopened SRAM");
}
void mapper(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=image();auto record=add(db,rom,settings());DbSramStore store(db,record);auto ram=std::get<std::vector<uint8_t>>(store.load());ram[0]=0x55;
    const RomUpdateContext ctx{rom,48000,{}, {}};auto cm=settings();cm.mapper=MapperKind::Codemasters;checkpoint(db,record.id,settings());
    require(update(store,record,ram,cm,&ctx).deleted==1 && ram==std::vector<uint8_t>(8192) && !store.enabled(),"Sega to CM");
    ram[0]=0xaa;update(store,record,ram,settings(16384),&ctx);require(ram==std::vector<uint8_t>(16384) && store.enabled(),"CM to Sega zero");
    update(store,record,ram,settings(0),&ctx);require(ram.empty() && !store.enabled(),"zero capacity");
    auto inactive=record.settings;inactive.fmPlugin=L"missing.dll";inactive.interpolation=FmInterpolation::Linear;const auto cp=checkpoint(db,record.id,record.settings);
    require(update(store,record,ram,inactive,&ctx).kept==1 && encodeState(cp.profile,*read(db,record.id,cp.id,cp.profile).state)==cp.bytes,"inactive FM settings deleted state");
    auto gg=settings();gg.machine=Machine::GameGear;gg.mapper=MapperKind::Codemasters;
    require(update(store,record,ram,gg,&ctx).deleted==1 && ram==std::vector<uint8_t>(8192),"multiple machine/mapper settings");
    checkpoint(db,record.id,gg);auto pal=gg;pal.machine=Machine::Sms;pal.video=VideoStandard::Pal;
    require(update(store,record,ram,pal,&ctx).deleted==1 && record.settings==pal,"GG to PAL SMS");
}
acesms_fm_api originalApi;
acesms_fm_result ACESMS_FM_CALL rejectSavedState(acesms_fm_instance* instance,const uint8_t* data,uint32_t size,const acesms_fm_load_context* context) {
    return context->master_ticks?ACESMS_FM_INVALID_STATE:originalApi.load_state(instance,data,size,context);
}
void fm(const std::filesystem::path& dir,const FmBackendPtr& external) {
    auto db=database(dir);const auto rom=image();auto cfg=settings();cfg.fmEnabled=true;auto record=add(db,rom,cfg);DbSramStore store(db,record);auto ram=std::get<std::vector<uint8_t>>(store.load());
    const auto builtin=FmBackendDefinition::builtin();const std::array<FmBackendPtr,2> backends{builtin,external};RomUpdateContext ctx{rom,48000,backends,builtin};
    const auto cp=checkpoint(db,record.id,cfg,builtin);auto next=cfg;next.fmPlugin=L"renamed.dll";
    const auto base=stateFileBytes(cp.profile)-67-cp.profile.fm->backendBytes-FmResampler::stateBytes(cp.profile.fm->method);
    for(size_t offset:{base+36,base+56,base+59}) {
        auto bad=cp.bytes;bad[offset]=0xff;bad[offset+1]=0xff;bad[offset+2]=0xff;bad[offset+3]=0xff;
        require(std::holds_alternative<StateCodecError>(inspectStateProfile(bad)),"invalid FM profile boundary");
    }
    require(update(store,record,ram,next,&ctx).kept==1 && encodeState(cp.profile,*read(db,record.id,cp.id,cp.profile).state)==cp.bytes,"DLL name used as identity");
    next.interpolation=FmInterpolation::Linear;require(update(store,record,ram,next,&ctx).deleted==1,"interpolation incompatible");
    checkpoint(db,record.id,next,builtin);ctx.targetBackend=external;require(update(store,record,ram,next,&ctx).deleted==1,"same filename backend identity changed");
    checkpoint(db,record.id,next,builtin);originalApi=builtin->api();auto api=originalApi;api.load_state=rejectSavedState;
    ctx.targetBackend=std::get<FmBackendPtr>(FmBackendDefinition::fromApi(&api));
    const auto before=fileBytes(dir);const auto previous=record;const auto oldRam=ram;
    require(std::holds_alternative<DatabaseError>(store.updateRom(record,ram,next,L"target reject",{},&ctx)) && fileBytes(dir)==before && record==previous && ram==oldRam,"kept state not checked with target backend");
    next.ramBytes=16384;
    require(std::holds_alternative<DatabaseError>(store.updateRom(record,ram,next,L"target reject",{},&ctx)) && fileBytes(dir)==before && record==previous && ram==oldRam,"converted state not checked with target backend");
}
void failures(const std::filesystem::path& dir) {
    for(size_t size:{0u,37u,41u}) require(std::holds_alternative<StateCodecError>(inspectStateProfile(std::vector<uint8_t>(size))),"short profile header");
    for(std::string_view fault:{"short","corrupt","opaque","missing_backend","metadata_sql","settings_sql","sram_sql","state_sql","commit"}) {
        auto db=database(dir);const auto rom=image();auto record=add(db,rom,settings());DbSramStore store(db,record);auto ram=std::get<std::vector<uint8_t>>(store.load());const auto baseline=ram;ram[0]=0xa7;const auto dirty=ram;const auto oldRecord=record;
        checkpoint(db,record.id,settings());auto source=settings();source.video=VideoStandard::Pal;
        const auto builtin=FmBackendDefinition::builtin();if(fault=="opaque" || fault=="missing_backend") {source.video=VideoStandard::Ntsc;source.fmEnabled=true;}
        const auto cp=checkpoint(db,record.id,source,builtin);
        if(fault=="short") seed(db,cp,std::array<uint8_t,1>{0});
        if(fault=="corrupt") {auto bytes=cp.bytes;bytes.back()^=1;seed(db,cp,bytes);}
        if(fault=="opaque") {auto decoded=decodeState(cp.bytes,cp.profile);auto& s=*std::get<std::unique_ptr<EmulatorSystemState>>(decoded);s.audio.fm->backend[0]^=1;seed(db,cp,encodeState(cp.profile,s));}
        const char* table=fault=="metadata_sql"?"roms":fault=="settings_sql"?"rom_settings":fault=="sram_sql"?"sram":nullptr;
        if(table) require(!db.execute((std::string("CREATE TRIGGER fail BEFORE UPDATE ON ")+table+" BEGIN SELECT RAISE(ABORT,'fixture');END").c_str()),"SQL trigger");
        if(fault=="state_sql") require(!db.execute("CREATE TRIGGER fail BEFORE DELETE ON states WHEN OLD.state_id=2 BEGIN SELECT RAISE(ABORT,'fixture');END"),"state trigger");
        const std::array<FmBackendPtr,1> backends{builtin};RomUpdateContext ctx{rom,48000,fault=="missing_backend"?std::span<const FmBackendPtr>{}:std::span<const FmBackendPtr>{backends},{}};
        sqlite3* raw=nullptr;require(sqlite3_open16((dir/L"update.sqlite3").c_str(),&raw)==SQLITE_OK,"reader");std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> reader(raw,sqlite3_close_v2);
        if(fault=="commit") require(sqlite3_exec(raw,"BEGIN;SELECT * FROM states",nullptr,nullptr,nullptr)==SQLITE_OK,"lock");
        const auto before=fileBytes(dir);auto result=store.updateRom(record,ram,settings(16384),L"new",L"new.sms",&ctx);require(std::holds_alternative<DatabaseError>(result),"failure accepted");
        if(fault=="short") require(std::get<DatabaseError>(result).message=="Invalid saved state BLOB length","short BLOB reached state profile decoder");
        if(fault=="commit") {require(std::get<DatabaseError>(result).code==SQLITE_BUSY,"commit failure");require(sqlite3_exec(raw,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"unlock");}
        require(fileBytes(dir)==before && record==oldRecord && ram==dirty && sqlite3_get_autocommit(db.nativeHandle()),"not atomic");
        {Trace trace(db);require(!store.save(baseline) && !trace.count,"failure changed baseline");}
        if(table || fault=="state_sql") require(!db.execute("DROP TRIGGER fail"),"drop trigger");seed(db,cp,cp.bytes);ctx.backends=backends;
        const auto counts=update(store,record,ram,settings(16384),&ctx);require(counts.converted==1 && counts.deleted==1,"retry classification");
        {Trace trace(db);require(!store.save(ram) && !trace.count,"retry baseline");}
    }
    auto db=database(dir);
    const auto rom=image();
    const auto record=add(db,rom,settings());
    DbSramStore store(db,record);
    const auto current=std::get<std::vector<uint8_t>>(store.load());
    const auto different=std::get<RomImage>(RomLoader::fromBytes(std::vector<uint8_t>(16384),{MapperKind::Sega}));
    const RomUpdateContext context{different,48000,{},{}};
    auto next=settings(0);
    next.mapper=MapperKind::Codemasters;
    const auto before=fileBytes(dir);
    const auto result=store.prepareUpdate(record,current,next,L"new",L"new.sms",&context);
    require(std::holds_alternative<DatabaseError>(result) &&
        std::get<DatabaseError>(result).kind==DatabaseErrorKind::InvalidData &&
        fileBytes(dir)==before,"mismatched context ROM was not rejected at the DB boundary");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3 || argc==4,"arguments");const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);const std::string_view op=argv[1];
        if(op=="metadata") metadata(dir);else if(op=="mixed") mixed(dir);else if(op=="mapper") mapper(dir);else if(op=="failures") failures(dir);
        else if(op=="fm") {require(argc==4,"DLL path");const auto path=std::filesystem::absolute(argv[3]);auto loaded=loadFmPlugin(path.parent_path(),path.filename());require(std::holds_alternative<FmBackendPtr>(loaded),"plugin");fm(dir,std::get<FmBackendPtr>(loaded));}else require(false,"operation");return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
