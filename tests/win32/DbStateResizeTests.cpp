#include "win32/DbSramStore.h"
#include "win32/EmulationSession.h"
#include "win32/FmPlugin.h"
#include "core/cartridge/RomLoader.h"
#include "../system/Stage8TestRom.h"
#include <sqlite3.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace smsgg;
using namespace smsgg::win32;
void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
RomImage image(uint32_t ram,uint8_t tag=0) {
    auto bytes=test::stage8FmRom();bytes.resize(32768);bytes.back()=tag;
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::Sega,ram}));
}
RomDatabase database(const std::filesystem::path& dir) {
    const auto path=dir/L"resize.sqlite3";std::filesystem::remove(path);
    auto opened=RomDatabase::open(path);require(std::holds_alternative<RomDatabase>(opened),"open");return std::get<RomDatabase>(std::move(opened));
}
RomRecord add(RomDatabase& db,const RomImage& rom,bool fm=false,FmInterpolation method=FmInterpolation::Sinc,bool external=false) {
    RomSettings settings;settings.ramBytes=static_cast<uint32_t>(rom.options().sramBytes);settings.fmEnabled=fm;settings.interpolation=method;
    if(external) settings.fmPlugin=L"acesms-fm-ymfm.dll";
    auto result=db.registerRom(std::get<RomIdentity>(identifyRom(rom)),settings,L"ROM",{});
    require(std::holds_alternative<RomRecord>(result),"register");return std::get<RomRecord>(std::move(result));
}
std::unique_ptr<EmulatorSystemState> capture(const EmulatorSystem& system) {auto state=std::make_unique<EmulatorSystemState>();system.captureState(*state);return state;}
void advance(EmulatorSystem& system,unsigned count=1500) {for(unsigned n=0;n<count;++n) require(std::holds_alternative<uint32_t>(system.step()),"step");}
int64_t save(RomDatabase& db,int64_t romId,const StateProfile& p,const EmulatorSystemState& s) {
    auto result=db.saveState(romId,L"same name",p,s);require(std::holds_alternative<int64_t>(result),"save");return std::get<int64_t>(result);
}
SavedState read(RomDatabase& db,int64_t romId,int64_t id,const StateProfile& p) {
    auto result=db.readState(romId,id,p);require(std::holds_alternative<SavedState>(result),"read converted");return std::get<SavedState>(std::move(result));
}
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement statement(RomDatabase& db,const char* sql) {sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db.nativeHandle(),sql,-1,&s,nullptr)==SQLITE_OK,"prepare");return Statement(s,sqlite3_finalize);}
void seed(RomDatabase& db,int64_t id,std::span<const uint8_t> bytes) {
    auto s=statement(db,"UPDATE states SET data=?1 WHERE state_id=?2");
    require(sqlite3_bind_blob(s.get(),1,bytes.data(),static_cast<int>(bytes.size()),SQLITE_STATIC)==SQLITE_OK && sqlite3_bind_int64(s.get(),2,id)==SQLITE_OK && sqlite3_step(s.get())==SQLITE_DONE,"seed");
}
std::string fileBytes(const std::filesystem::path& dir) {std::ifstream file(dir/L"resize.sqlite3",std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
struct Trace {
    int count{};sqlite3* db;
    static int call(unsigned,void* p,void*,void*) {++static_cast<Trace*>(p)->count;return 0;}
    explicit Trace(RomDatabase& d):db(d.nativeHandle()) {require(sqlite3_trace_v2(db,SQLITE_TRACE_STMT,call,this)==SQLITE_OK,"trace");}
    ~Trace(){sqlite3_trace_v2(db,0,nullptr,nullptr);}
};
void conversion(const std::filesystem::path& dir,bool fm,FmInterpolation method,const FmBackendPtr& backend,bool external,uint32_t oldSize,uint32_t newSize) {
    auto db=database(dir);const auto rom=image(oldSize);const auto record=add(db,rom,fm,method,external);
    EmulationSession current(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,fm,method,backend);
    const StateResizeContext context{rom,current.profile(),backend};
    DbSramStore store(db,record);auto loaded=store.load();require(std::holds_alternative<std::vector<uint8_t>>(loaded),"load SRAM");
    auto ram=std::get<std::vector<uint8_t>>(std::move(loaded));std::fill(ram.begin(),ram.end(),0xc3);
    struct Expected {int64_t id;StateProfile profile;std::vector<uint8_t> bytes;int64_t created,updated;};std::vector<Expected> expected;
    for(uint32_t rate:{44100u,96000u}) {
        EmulationSession saved(rom,dir/L"unused.sms",VideoStandard::Ntsc,rate,Machine::Sms,fm,method,backend);advance(saved.system());
        auto state=capture(saved.system());std::fill(state->cartridge.ram.begin(),state->cartridge.ram.end(),static_cast<uint8_t>(rate/1000));
        if(rate==96000) {state->vdp.scrollX=8;state->vdp.nextScrollX=16;}
        const auto id=save(db,record.id,saved.profile(),*state);const auto original=read(db,record.id,id,saved.profile());
        auto p=saved.profile();p.ramBytes=newSize;state->cartridge.ram.resize(newSize,0);
        expected.push_back({id,p,encodeState(p,*state),original.createdAt,original.updatedAt});
    }
    const auto otherRom=image(oldSize,1);const auto other=add(db,otherRom,fm,method,external);
    EmulationSession otherSession(otherRom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,fm,method,backend);
    auto otherState=capture(otherSession.system());const auto otherId=save(db,other.id,otherSession.profile(),*otherState);const auto otherBytes=encodeState(otherSession.profile(),*otherState);
    require(!store.resize(ram,newSize,&context),"resize with states");
    if(oldSize==newSize) require(!store.save(ram),"same capacity retains ordinary dirty SRAM save");
    require(ram.size()==newSize && std::all_of(ram.begin(),ram.begin()+std::min(oldSize,newSize),[](uint8_t v){return v==0xc3;}) &&
        std::all_of(ram.begin()+std::min(oldSize,newSize),ram.end(),[](uint8_t v){return v==0;}),"latest SRAM independently resized");
    {Trace trace(db);require(!store.save(ram) && trace.count==0,"converted baseline");}
    auto found=db.findRom(record.identity);const auto updated=std::get<std::optional<RomRecord>>(found);require(updated && updated->settings.ramBytes==newSize,"settings capacity");
    auto reopened=RomDatabase::open(dir/L"resize.sqlite3");require(std::holds_alternative<RomDatabase>(reopened),"reopen");auto& again=std::get<RomDatabase>(reopened);
    DbSramStore sramAgain(again,*updated);require(std::get<std::vector<uint8_t>>(sramAgain.load())==ram,"SRAM reopen");
    const auto target=image(newSize);
    for(const auto& e:expected) {
        const auto restored=read(again,record.id,e.id,e.profile);
        require(restored.id==e.id && restored.name==L"same name" && restored.createdAt==e.created && restored.updatedAt==e.updated && encodeState(e.profile,*restored.state)==e.bytes,"all fields except RAM preserved");
        auto candidate=std::make_unique<EmulatorSystem>(target,VideoStandard::Ntsc,e.profile.sampleRate,Machine::Sms,fm,method,backend);
        auto reference=std::make_unique<EmulatorSystem>(target,VideoStandard::Ntsc,e.profile.sampleRate,Machine::Sms,fm,method,backend);
        const auto decoded=decodeState(e.bytes,e.profile);require(candidate->restoreState(*restored.state)==ACESMS_FM_OK && reference->restoreState(*std::get<std::unique_ptr<EmulatorSystemState>>(decoded))==ACESMS_FM_OK,"restore candidate");
        for(unsigned n=0;n<100;++n) {
            advance(*candidate,1);advance(*reference,1);const auto a=candidate->audioFrames(),b=reference->audioFrames();
            require(std::equal(a.begin(),a.end(),b.begin(),b.end(),[](const auto& x,const auto& y){return x.left==y.left && x.right==y.right;}),"continued PCM");
        }
        require(encodeState(e.profile,*capture(*candidate))==encodeState(e.profile,*capture(*reference)),"continued candidate");
    }
    require(encodeState(otherSession.profile(),*read(db,other.id,otherId,otherSession.profile()).state)==otherBytes,"other ROM preserved");
    auto ordinary=current.profile();ordinary.ramBytes=newSize;require(std::holds_alternative<StateCodecError>(db.readState(record.id,expected[0].id,ordinary)),"ordinary load keeps strict rate");
}
void failures(const std::filesystem::path& dir) {
    for(size_t size:{0u,34u,37u,41u}) {
        auto result=decodeStateAtSavedRate(std::vector<uint8_t>(size),{});
        require(std::holds_alternative<StateCodecError>(result) && std::get<StateCodecError>(result)==StateCodecError::Size,"short saved-rate header");
    }
    for(std::string_view fault:{"crc","profile","rate_zero","rate_high","opaque","backend","sql","commit"}) {
        auto db=database(dir);const auto rom=image(8192);const auto record=add(db,rom,true);const auto backend=FmBackendDefinition::builtin();
        EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,true,FmInterpolation::Sinc,backend);advance(session.system());
        auto state=capture(session.system());save(db,record.id,session.profile(),*state);const auto id=save(db,record.id,session.profile(),*state);
        auto p=session.profile();auto bytes=encodeState(p,*state);
        if(fault=="crc") {bytes.back()^=1;seed(db,id,bytes);}
        if(fault=="profile") {p.machine=Machine::GameGear;seed(db,id,encodeState(p,*state));}
        if(fault=="rate_zero" || fault=="rate_high") {
            p.sampleRate=fault=="rate_zero"?0:4000000;seed(db,id,encodeState(p,*state));
        }
        if(fault=="opaque") {state->audio.fm->backend[0]^=1;seed(db,id,encodeState(p,*state));}
        if(fault=="sql") require(!db.execute("CREATE TRIGGER fail_second BEFORE UPDATE ON states WHEN OLD.state_id=2 BEGIN SELECT RAISE(ABORT,'fixture');END"),"trigger");
        DbSramStore store(db,record);auto current=std::get<std::vector<uint8_t>>(store.load());const auto baseline=current;current[0]=0xa7;const auto dirty=current;
        const StateResizeContext context{rom,session.profile(),fault=="backend"?FmBackendPtr{}:backend};
        sqlite3* raw=nullptr;require(sqlite3_open16((dir/L"resize.sqlite3").c_str(),&raw)==SQLITE_OK,"reader");std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> reader(raw,sqlite3_close_v2);
        if(fault=="commit") require(sqlite3_exec(raw,"BEGIN;SELECT * FROM states",nullptr,nullptr,nullptr)==SQLITE_OK,"lock");
        const auto before=fileBytes(dir);auto error=store.resize(current,16384,&context);require(error.has_value(),"invalid conversion accepted");
        if(fault=="commit") {require(error->code==SQLITE_BUSY,"commit error");require(sqlite3_exec(raw,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"unlock");}
        require(current==dirty && fileBytes(dir)==before && sqlite3_get_autocommit(db.nativeHandle()),"whole database and RAM rollback");
        {Trace trace(db);require(!store.save(baseline) && trace.count==0,"failed conversion advanced baseline");}
        if(fault=="sql") require(!db.execute("DROP TRIGGER fail_second"),"drop trigger");
        state->audio.fm->backend=capture(session.system())->audio.fm->backend;seed(db,id,encodeState(session.profile(),*state));
        const StateResizeContext retry{rom,session.profile(),backend};require(!store.resize(current,16384,&retry),"explicit retry");
        {Trace trace(db);require(!store.save(current) && trace.count==0,"retry baseline");}
    }
}
void legacy(const std::filesystem::path& dir) {
    auto db=database(dir);
    const auto rom=image(8192);
    const auto record=add(db,rom,true);
    EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,true);
    advance(session.system());
    const auto& p=session.profile();
    auto state=capture(session.system());
    state->cartridge.ram[4]=0x67;
    const auto expected=encodeState(p,*state);
    auto old=expected;
    old.erase(old.begin()+8288+8192+4); // Previous layout has no 2P byte after the Sega mapper and controller fields.
    old[8]=2;
    for(unsigned n=0;n<4;++n) old[10+n]=static_cast<uint8_t>((old.size()-42)>>(8*n));
    std::fill_n(old.begin()+14,4,0);
    uint32_t crc=~0u;
    for(const auto byte:old) {
        crc^=byte;
        for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    for(unsigned n=0;n<4;++n) old[14+n]=static_cast<uint8_t>((~crc)>>(8*n));
    const auto id=save(db,record.id,p,*state);
    seed(db,id,old);
    require(std::holds_alternative<DatabaseError>(db.readState(record.id,id,p)),"old DB state rejected");
    require(!writeFileAtomically(dir/L"legacy.state",old),"legacy file fixture");
    auto file=session.prepareState(dir/L"legacy.state");
    require(std::holds_alternative<SessionError>(file),"old state file rejected");
    DbSramStore store(db,record);
    auto ram=std::get<std::vector<uint8_t>>(store.load());
    const StateResizeContext context{rom,p,FmBackendDefinition::builtin()};
    require(store.resize(ram,16384,&context).has_value(),"old DB state conversion rejected");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3 || argc==4,"arguments");const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);const std::string_view op=argv[1];
        FmBackendPtr backend=FmBackendDefinition::builtin();if(argc==4) {const auto path=std::filesystem::absolute(argv[3]);auto loaded=loadFmPlugin(path.parent_path(),path.filename());require(std::holds_alternative<FmBackendPtr>(loaded),"plugin");backend=std::get<FmBackendPtr>(std::move(loaded));}
        if(op=="matrix") for(uint32_t a:{0u,8192u,16384u,32768u}) for(uint32_t b:{0u,8192u,16384u,32768u}) conversion(dir,false,FmInterpolation::Sinc,{},false,a,b);
        else if(op=="fm") for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear}) conversion(dir,true,method,backend,argc==4,8192,16384);
        else if(op=="failures") failures(dir);
        else if(op=="legacy") legacy(dir);
        else require(false,"operation");return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
