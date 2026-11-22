#include "win32/DbSramStore.h"
#include "core/cartridge/RomLoader.h"
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using namespace smsgg;
using namespace smsgg::win32;
void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
RomDatabase database(const std::filesystem::path& dir) {
    const auto path=dir/L"sram.sqlite3";std::filesystem::remove(path);
    auto result=RomDatabase::open(path);require(std::holds_alternative<RomDatabase>(result),"open DB");return std::get<RomDatabase>(std::move(result));
}
RomRecord add(RomDatabase& db,uint8_t tag,uint32_t size,MapperKind mapper=MapperKind::Sega) {
    std::vector<uint8_t> bytes(32768,tag);
    const auto rom=std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::Auto}));
    const auto id=std::get<RomIdentity>(identifyRom(rom));
    RomSettings settings;settings.mapper=mapper;settings.ramBytes=size;
    auto result=db.registerRom(id,settings,L"SRAM fixture",L"not-opened.gg");
    if(auto* error=std::get_if<DatabaseError>(&result)) throw std::runtime_error(error->message);
    return std::get<RomRecord>(std::move(result));
}
std::vector<uint8_t> load(DbSramStore& store) {
    auto result=store.load();
    if(auto* error=std::get_if<DatabaseError>(&result)) throw std::runtime_error(error->message);
    return std::get<std::vector<uint8_t>>(std::move(result));
}
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement statement(RomDatabase& db,const char* sql,int64_t id) {
    sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db.nativeHandle(),sql,-1,&s,nullptr)==SQLITE_OK,"fixture prepare");
    Statement owner(s,sqlite3_finalize);require(sqlite3_bind_int64(s,1,id)==SQLITE_OK,"fixture id");return owner;
}
std::vector<uint8_t> stored(RomDatabase& db,int64_t id) {
    auto s=statement(db,"SELECT data FROM sram WHERE rom_id=?1",id);require(sqlite3_step(s.get())==SQLITE_ROW,"stored row");
    const auto n=sqlite3_column_bytes(s.get(),0);const auto* p=static_cast<const uint8_t*>(sqlite3_column_blob(s.get(),0));
    return n?std::vector<uint8_t>(p,p+n):std::vector<uint8_t>{};
}
void seed(RomDatabase& db,int64_t id,std::span<const uint8_t> bytes) {
    auto s=statement(db,"INSERT OR REPLACE INTO sram(rom_id,data) VALUES(?1,?2)",id);
    const auto code=bytes.empty()?sqlite3_bind_zeroblob(s.get(),2,0):sqlite3_bind_blob(s.get(),2,bytes.data(),static_cast<int>(bytes.size()),SQLITE_STATIC);
    require(code==SQLITE_OK && sqlite3_step(s.get())==SQLITE_DONE,"seed blob");
}
int count(RomDatabase& db,const char* sql) {
    sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db.nativeHandle(),sql,-1,&s,nullptr)==SQLITE_OK,"count prepare");
    Statement owner(s,sqlite3_finalize);require(sqlite3_step(s)==SQLITE_ROW,"count step");return sqlite3_column_int(s,0);
}
struct Trace {
    RomDatabase& db;int count{};
    static int call(unsigned,void* context,void*,void*) {++static_cast<Trace*>(context)->count;return 0;}
    explicit Trace(RomDatabase& d):db(d) {require(sqlite3_trace_v2(db.nativeHandle(),SQLITE_TRACE_STMT,call,this)==SQLITE_OK,"trace");}
    ~Trace(){sqlite3_trace_v2(db.nativeHandle(),0,nullptr,nullptr);}
};
void roundtrip(const std::filesystem::path& dir) {
    auto db=database(dir);uint8_t tag=0;
    for(uint32_t size:{8192u,16384u,32768u}) {
        const auto rom=add(db,++tag,size);DbSramStore store(db,rom);require(store.enabled(),"Sega SRAM disabled");
        const std::vector<uint8_t> zero(size);require(load(store)==zero && stored(db,rom.id)==zero,"registration zero blob");
        auto data=load(store);for(size_t i=0;i<data.size();++i)data[i]=static_cast<uint8_t>(i*37+13);data.back()=0xff;
        require(!store.save(data) && stored(db,rom.id)==data,"SRAM roundtrip");
        auto opened=RomDatabase::open(dir/L"sram.sqlite3");require(std::holds_alternative<RomDatabase>(opened),"reopen DB");
        auto& again=std::get<RomDatabase>(opened);DbSramStore second(again,rom);require(load(second)==data,"reopened full bytes");
        const auto repeated=add(db,tag,size);require(repeated.id==rom.id && stored(db,rom.id)==data,"re-register reset SRAM");
    }
}
void baseline(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=add(db,0,8192);DbSramStore store(db,rom);auto a=load(store);auto b=a;b[7]=0x5a;
    Trace trace(db);
    require(!store.save(a) && trace.count==0,"unchanged zero performed SQL");
    require(!store.save(b) && trace.count>0,"changed bytes skipped SQL");
    trace.count=0;require(!store.save(b) && trace.count==0,"successful save did not update baseline");
    require(!store.save(a) && trace.count>0,"return to old value skipped save");
    trace.count=0;require(!store.save(a) && trace.count==0,"second baseline");
    auto copy=load(store);copy[0]=0x77;trace.count=0;
    require(!store.save(copy) && trace.count>0,"load result aliases baseline");
    require(stored(db,rom.id)==copy,"copy content");
}
void invalid(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=add(db,0,8192);DbSramStore store(db,rom);auto a=load(store);a[4]=0x31;require(!store.save(a),"baseline seed");
    require(!db.execute("DELETE FROM sram"),"delete row");
    require(std::holds_alternative<DatabaseError>(store.load()),"missing row zero initialized");
    auto b=a;b[5]=0x32;require(store.save(b).has_value(),"missing row recreated on save");
    seed(db,rom.id,a);
    {Trace trace(db);require(!store.save(a) && trace.count==0,"missing/read failure changed baseline");}
    for(size_t size:{0u,8191u,8193u,16384u}) {
        const std::vector<uint8_t> bad(size,0x24);seed(db,rom.id,bad);
        auto failed=store.load();require(std::holds_alternative<DatabaseError>(failed) && std::get<DatabaseError>(failed).kind==DatabaseErrorKind::InvalidData,"invalid length accepted");
        require(store.save(b).has_value() && stored(db,rom.id)==bad,"wrong length resized on save");
        seed(db,rom.id,a);Trace trace(db);require(!store.save(a) && trace.count==0,"invalid length changed baseline");
    }
    require(!db.execute("ALTER TABLE sram RENAME TO held_sram"),"rename fixture");
    auto error=store.load();require(std::holds_alternative<DatabaseError>(error) && std::get<DatabaseError>(error).kind==DatabaseErrorKind::Sqlite,"SQL read error missing");
    require(!db.execute("ALTER TABLE held_sram RENAME TO sram"),"restore fixture");
    const auto typeError=db.execute("UPDATE sram SET data='text is not SRAM'");
    require(typeError && typeError->code==SQLITE_CONSTRAINT && stored(db,rom.id)==a,"non-BLOB storage accepted");
    Trace trace(db);require(!store.save(a) && trace.count==0,"SQL read failure changed baseline");
}
void disabled(const std::filesystem::path& dir) {
    auto db=database(dir);uint8_t tag=0;
    for(const auto& settings:std::array<std::pair<MapperKind,uint32_t>,4>{{{MapperKind::RomOnly,0},{MapperKind::Sega,0},{MapperKind::Codemasters,0},{MapperKind::Codemasters,8192}}}) {
        const auto rom=add(db,++tag,settings.second,settings.first);DbSramStore store(db,rom);require(!store.enabled(),"nonpersistent SRAM enabled");
        Trace trace(db);require(load(store).empty() && !store.save(std::array<uint8_t,4>{1,2,3,4}) && trace.count==0,"disabled issued SQL");
    }
    require(count(db,"SELECT count(*) FROM sram")==0,"disabled SRAM rows");
}
void failures(const std::filesystem::path& dir) {
    auto db=database(dir);
    require(!db.execute("CREATE TRIGGER fail_registration BEFORE INSERT ON sram BEGIN SELECT RAISE(ABORT,'fixture');END"),"registration trigger");
    auto romImage=std::get<RomImage>(RomLoader::fromBytes(std::vector<uint8_t>(32768),{MapperKind::Sega}));
    RomSettings settings;settings.ramBytes=8192;
    auto failed=db.registerRom(std::get<RomIdentity>(identifyRom(romImage)),settings,L"fixture",{});
    require(std::holds_alternative<DatabaseError>(failed) && count(db,"SELECT count(*) FROM roms")==0 && count(db,"SELECT count(*) FROM rom_settings")==0 && count(db,"SELECT count(*) FROM sram")==0,"partial SRAM registration");
    require(!db.execute("DROP TRIGGER fail_registration"),"drop registration trigger");
    const auto rom=add(db,0,8192);DbSramStore store(db,rom);auto a=load(store);a[0]=0x11;require(!store.save(a),"initial save");
    auto b=a;b[0]=0x22;
    require(!db.execute("CREATE TRIGGER fail_update BEFORE UPDATE ON sram BEGIN SELECT RAISE(ABORT,'fixture');END"),"update trigger");
    require(store.save(b).has_value() && stored(db,rom.id)==a,"SQL failure changed blob");
    require(!db.execute("DROP TRIGGER fail_update") && !store.save(b) && stored(db,rom.id)==b,"SQL failure advanced baseline");
    sqlite3* reader=nullptr;require(sqlite3_open16((dir/L"sram.sqlite3").c_str(),&reader)==SQLITE_OK,"reader");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> owner(reader,sqlite3_close_v2);
    require(sqlite3_exec(reader,"BEGIN;SELECT * FROM sram",nullptr,nullptr,nullptr)==SQLITE_OK,"read lock");
    auto c=b;c[0]=0x33;auto busy=store.save(c);require(busy && busy->code==SQLITE_BUSY,"commit failure missing");
    require(sqlite3_exec(reader,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"unlock");
    require(sqlite3_get_autocommit(db.nativeHandle()) && stored(db,rom.id)==b && !store.save(c) && stored(db,rom.id)==c,"commit failure baseline/rollback");
    auto d=c;d[0]=0x44;require(!db.execute("PRAGMA query_only=ON"),"readonly fixture");auto ro=store.save(d);require(ro && ro->code==SQLITE_READONLY,"readonly save");
    require(!db.execute("PRAGMA query_only=OFF") && stored(db,rom.id)==c && !store.save(d) && stored(db,rom.id)==d,"readonly baseline");
    Trace trace(db);require(!store.save(d) && trace.count==0,"final unchanged save");
}
void resize(const std::filesystem::path& dir) {
    auto db=database(dir);uint8_t tag=0;
    for(uint32_t oldSize:{0u,8192u,16384u,32768u}) for(uint32_t size:{0u,8192u,16384u,32768u}) {
        const auto rom=add(db,++tag,oldSize);DbSramStore store(db,rom);auto current=load(store);
        for(size_t i=0;i<current.size();++i) current[i]=static_cast<uint8_t>(i*37+11);
        auto expected=current;expected.resize(size,0);
        {Trace trace(db);require(!store.resize(current,size) && current==expected,"resize latest RAM");if(size==oldSize) require(trace.count==0,"same capacity performed SQL");}
        require(store.enabled()==(size!=0),"enabled after resize");
        if(size==oldSize) require(!store.save(current),"same capacity remains ordinary dirty save");
        auto found=db.findRom(rom.identity);require(std::holds_alternative<std::optional<RomRecord>>(found),"settings read");
        const auto updated=std::get<std::optional<RomRecord>>(found);require(updated.has_value(),"settings missing");
        auto settings=rom.settings;settings.ramBytes=size;require(updated->settings==settings,"only capacity changed");
        if(size) require(stored(db,rom.id)==expected,"resize disk bytes");
        else {auto s=statement(db,"SELECT count(*) FROM sram WHERE rom_id=?1",rom.id);require(sqlite3_step(s.get())==SQLITE_ROW && sqlite3_column_int(s.get(),0)==0,"zero row not deleted");}
        {Trace trace(db);require(!store.save(current) && trace.count==0,"resize baseline");}
        auto opened=RomDatabase::open(dir/L"sram.sqlite3");require(std::holds_alternative<RomDatabase>(opened),"resize reopen");
        DbSramStore again(std::get<RomDatabase>(opened),*updated);require(load(again)==expected,"resize reopened bytes");
    }
    const auto rom=add(db,++tag,8192);DbSramStore store(db,rom);auto current=load(store);current[0]=0x7b;
    {Trace trace(db);auto error=store.resize(current,4096);require(error && error->kind==DatabaseErrorKind::InvalidSettings && trace.count==0 && current[0]==0x7b,"invalid capacity");}
    require(!store.resize(current,0) && current.empty() && !store.enabled(),"to zero");
    require(!store.resize(current,8192) && current==std::vector<uint8_t>(8192),"zero to nonzero does not resurrect RAM");
    for(auto mapper:{MapperKind::RomOnly,MapperKind::Codemasters}) {
        const auto record=add(db,++tag,mapper==MapperKind::Codemasters?8192:0,mapper);DbSramStore disabledStore(db,record);
        auto ram=std::vector<uint8_t>(record.settings.ramBytes,0x51);const auto before=ram;
        Trace trace(db);auto error=disabledStore.resize(ram,8192);
        require(error && error->kind==DatabaseErrorKind::InvalidSettings && !disabledStore.clear(ram) && trace.count==0 && ram==before,"nonpersistent management");
    }
}
void clear(const std::filesystem::path& dir) {
    auto db=database(dir);uint8_t tag=0;
    for(uint32_t size:{8192u,16384u,32768u}) {
        const auto rom=add(db,++tag,size);DbSramStore store(db,rom);auto current=load(store);std::fill(current.begin(),current.end(),0x71);
        require(!store.save(current),"clear seed");current.back()=0xa3;
        auto image=std::get<RomImage>(RomLoader::fromBytes(std::vector<uint8_t>(32768,tag),{MapperKind::Sega,size}));
        EmulatorSystem system(image,VideoStandard::Ntsc,48000);auto state=std::make_unique<EmulatorSystemState>();system.captureState(*state);state->cartridge.ram=current;
        const StateProfile p{rom.identity.bytes,rom.identity.crc32,Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,size,48000,{}};
        auto saved=db.saveState(rom.id,L"preserve",p,*state);require(std::holds_alternative<int64_t>(saved),"clear state save");
        const auto before=encodeState(p,*state);const auto original=current;
        require(store.resize(current,size==8192?16384:8192).has_value() && current==original && stored(db,rom.id)!=std::vector<uint8_t>(size),"states prevent partial resize");
        require(!store.clear(current) && current==std::vector<uint8_t>(size) && stored(db,rom.id)==current,"clear all bytes");
        {Trace trace(db);require(!store.save(current) && !store.clear(current) && trace.count==0,"clear baseline or repeated clear");}
        current[0]=0xee;{Trace trace(db);require(!store.clear(current) && trace.count==0 && current==std::vector<uint8_t>(size),"clear dirty RAM over zero baseline");}
        auto loaded=db.readState(rom.id,std::get<int64_t>(saved),p);require(std::holds_alternative<SavedState>(loaded),"state retained");
        const auto& preserved=std::get<SavedState>(loaded);require(preserved.name==L"preserve" && encodeState(p,*preserved.state)==before,"clear changed state");
        require(system.restoreState(*preserved.state)==ACESMS_FM_OK,"explicit old state restore remains possible");
        current=preserved.state->cartridge.ram;
        {Trace trace(db);require(!store.save(current) && trace.count>0,"clear baseline skipped explicitly restored old RAM");}
        require(stored(db,rom.id)==original && !store.clear(current),"explicit restore then clear");
        auto record=db.findRom(rom.identity);require(std::get<std::optional<RomRecord>>(record)->settings==rom.settings,"clear changed capacity");
        auto opened=RomDatabase::open(dir/L"sram.sqlite3");require(std::holds_alternative<RomDatabase>(opened),"clear reopen");
        DbSramStore again(std::get<RomDatabase>(opened),rom);require(load(again)==current,"clear reopened bytes");
    }
}
void manageFailures(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=add(db,0,8192);DbSramStore store(db,rom);auto current=load(store);current[0]=0x11;
    require(!store.save(current),"management seed");const auto baseline=current;current[1]=0x22;const auto dirty=current;
    require(!db.execute("CREATE TRIGGER fail_resize BEFORE UPDATE ON sram BEGIN SELECT RAISE(ABORT,'fixture');END"),"resize trigger");
    require(store.resize(current,16384).has_value() && current==dirty && stored(db,rom.id)==baseline,"resize SQL rollback RAM");
    require(std::get<std::optional<RomRecord>>(db.findRom(rom.identity))->settings.ramBytes==8192,"resize SQL rollback settings");
    require(store.clear(current).has_value() && current==dirty,"clear SQL failure RAM");
    {Trace trace(db);require(!store.save(baseline) && trace.count==0,"failure advanced baseline");}
    require(!db.execute("DROP TRIGGER fail_resize") && !store.resize(current,16384),"resize retry");
    const auto expanded=current;
    sqlite3* raw=nullptr;require(sqlite3_open16((dir/L"sram.sqlite3").c_str(),&raw)==SQLITE_OK,"management reader");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> reader(raw,sqlite3_close_v2);
    require(sqlite3_exec(raw,"BEGIN;SELECT * FROM sram",nullptr,nullptr,nullptr)==SQLITE_OK,"management lock");
    auto failed=store.resize(current,8192);require(failed && failed->code==SQLITE_BUSY && current==expanded,"resize commit failure");
    failed=store.clear(current);require(failed && failed->code==SQLITE_BUSY && current==expanded,"clear commit failure");
    require(sqlite3_exec(raw,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"management unlock");
    require(sqlite3_get_autocommit(db.nativeHandle()) && stored(db,rom.id)==expanded,"commit rollback bytes");
    require(std::get<std::optional<RomRecord>>(db.findRom(rom.identity))->settings.ramBytes==16384,"commit rollback settings");
    {Trace trace(db);require(!store.save(current) && trace.count==0,"commit failure advanced baseline");}
    require(!store.resize(current,8192) && !store.clear(current),"management retry");
    current[0]=0x51;require(!store.save(current),"readonly seed");const auto a=current;
    require(!db.execute("PRAGMA query_only=ON"),"readonly management");
    failed=store.resize(current,16384);require(failed && failed->code==SQLITE_READONLY,"resize readonly");
    failed=store.clear(current);require(failed && failed->code==SQLITE_READONLY && current==a,"clear readonly");
    require(!db.execute("PRAGMA query_only=OFF"),"readonly release");
    require(!db.execute("UPDATE rom_settings SET mapper=4294967297"),"wide invalid mapper fixture");
    require(store.resize(current,16384).has_value() && current==a,"INTEGER mapper truncated to Sega");
    require(!db.execute("UPDATE rom_settings SET mapper=1"),"restore mapper");
    for(size_t length:{0u,8191u}) {
        seed(db,rom.id,std::vector<uint8_t>(length,0x33));require(store.resize(current,16384).has_value() && store.clear(current).has_value() && current==a,"invalid SRAM management");seed(db,rom.id,a);
    }
    require(!db.execute("DELETE FROM sram"),"missing SRAM fixture");
    require(store.resize(current,16384).has_value() && store.clear(current).has_value() && current==a,"missing SRAM management");seed(db,rom.id,a);
    require(!db.execute("UPDATE rom_settings SET ram_bytes=16384"),"stale settings fixture");
    require(store.resize(current,16384).has_value() && current==a,"settings mismatch repaired");require(!db.execute("UPDATE rom_settings SET ram_bytes=8192"),"restore settings");
    require(!db.execute("CREATE TRIGGER fail_delete BEFORE DELETE ON sram BEGIN SELECT RAISE(ABORT,'fixture');END"),"delete trigger");
    require(store.resize(current,0).has_value() && current==a,"delete failure");
    require(!db.execute("DROP TRIGGER fail_delete") && !store.resize(current,0),"delete retry");
    require(!db.execute("CREATE TRIGGER fail_insert BEFORE INSERT ON sram BEGIN SELECT RAISE(ABORT,'fixture');END"),"insert trigger");
    require(store.resize(current,8192).has_value() && current.empty() && !store.enabled(),"insert failure");
    require(std::get<std::optional<RomRecord>>(db.findRom(rom.identity))->settings.ramBytes==0,"insert failure settings");
    require(!db.execute("DROP TRIGGER fail_insert"),"insert release");seed(db,rom.id,std::vector<uint8_t>(8192));
    require(store.resize(current,8192).has_value() && current.empty(),"unexpected zero-capacity row accepted");
    require(!db.execute("DELETE FROM sram") && !store.resize(current,8192),"zero capacity retry");
    {Trace trace(db);require(!store.save(current) && trace.count==0,"management final baseline");}
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"arguments");const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);const std::string_view op=argv[1];
        if(op=="roundtrip") roundtrip(dir);else if(op=="baseline") baseline(dir);else if(op=="invalid") invalid(dir);
        else if(op=="disabled") disabled(dir);else if(op=="failures") failures(dir);else if(op=="resize") resize(dir);
        else if(op=="clear") clear(dir);else if(op=="manage_failures") manageFailures(dir);else throw std::runtime_error("operation");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
