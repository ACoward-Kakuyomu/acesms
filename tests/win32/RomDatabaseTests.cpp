#include "win32/RomDatabase.h"
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using smsgg::Machine;
using smsgg::SearchWidth;
using namespace smsgg::win32;
void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
using Raw=std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)>;
Raw raw(const std::filesystem::path& path) {
    sqlite3* db=nullptr;const auto code=sqlite3_open16(path.c_str(),&db);
    Raw result(db,sqlite3_close_v2);require(code==SQLITE_OK,"raw open");return result;
}
void sql(sqlite3* db,const char* text) {require(sqlite3_exec(db,text,nullptr,nullptr,nullptr)==SQLITE_OK,"fixture SQL");}
int scalar(sqlite3* db,const char* text) {
    sqlite3_stmt* statement=nullptr;
    require(sqlite3_prepare_v2(db,text,-1,&statement,nullptr)==SQLITE_OK,"prepare scalar");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owner(statement,sqlite3_finalize);
    require(sqlite3_step(statement)==SQLITE_ROW,"step scalar");return sqlite3_column_int(statement,0);
}
RomDatabase open(const std::filesystem::path& path) {
    auto result=RomDatabase::open(path);
    if(auto* error=std::get_if<DatabaseError>(&result)) throw std::runtime_error(error->message);
    return std::get<RomDatabase>(std::move(result));
}
std::filesystem::path fresh(const std::filesystem::path& dir,const wchar_t* name) {
    const auto path=dir/name;std::filesystem::remove(path);return path;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);return {(std::istreambuf_iterator<char>(input)),{}};
}
void opening(const std::filesystem::path& dir) {
    require(SQLITE_VERSION_NUMBER==3054001 && std::string_view(sqlite3_libversion())=="3.54.1","fixed version");
    require(std::string_view(sqlite3_sourceid())=="2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76","fixed source ID");
    const auto path=fresh(dir,L"\u65e5\u672c\u8a9e.sqlite3");
    {
        auto db=open(path);require(scalar(db.nativeHandle(),"PRAGMA user_version")==1,"new version");
        require(scalar(db.nativeHandle(),"PRAGMA foreign_keys")==1,"FK enabled");
        require(scalar(db.nativeHandle(),"SELECT count(*) FROM sqlite_schema WHERE type='table' AND name NOT GLOB 'sqlite_*'")==7,"ROM, SRAM, state, application settings and cheat tables");
        const auto defaults=std::get<AppSettings>(db.readAppSettings());
        require(!defaults.controllers && !defaults.fourThree && defaults.rom.options.sramBytes==32768,"fresh app defaults");
        auto rom=defaults.rom;
        rom.options.sramBytes=8192;
        rom.fmPlugin=L"\u65e5\u672c\u8a9e.dll";
        ControllerConfiguration pads{};
        pads[1].source=PadSource::XInput;
        pads[1].slot=2;
        pads[1].buttons[3]=0;
        require(!db.saveAppSettings(rom,pads) && !db.saveAspectRatio(true),"save app settings");
        sql(db.nativeHandle(),"CREATE TRIGGER reject_p2 BEFORE INSERT ON app_controllers WHEN NEW.player=2 BEGIN SELECT RAISE(ABORT,'controller test');END");
        rom.options.sramBytes=16384;
        pads[0].keys[0]=65;
        require(db.saveAppSettings(rom,pads).has_value(),"failed settings write accepted");
        require(sqlite3_get_autocommit(db.nativeHandle())!=0,"settings transaction did not roll back");
        auto moved=std::move(db);require(moved.nativeHandle()!=nullptr,"move owner");
        require(!moved.execute("CREATE TABLE preserved(value); INSERT INTO preserved VALUES(17)"),"write");
    }
    {
        auto db=open(path);require(scalar(db.nativeHandle(),"SELECT value FROM preserved")==17,"reopen content");
        require(scalar(db.nativeHandle(),"PRAGMA foreign_keys")==1,"reopen FK");
        const auto saved=std::get<AppSettings>(db.readAppSettings());
        require(saved.fourThree && saved.rom.options.sramBytes==8192 && saved.rom.fmPlugin==L"\u65e5\u672c\u8a9e.dll" &&
            saved.controllers && (*saved.controllers)[0].keys[0]==0 && (*saved.controllers)[1].slot==2 &&
            (*saved.controllers)[1].buttons[3]==0,"app settings reopen or rollback");
    }
    require(std::filesystem::remove(path),"connection released file");
}
void schema(const std::filesystem::path& dir) {
    const auto previousSchema=fresh(dir,L"before-cheats.sqlite3");
    {auto db=raw(previousSchema);sql(db.get(),"PRAGMA user_version=1;CREATE TABLE roms(id);CREATE TABLE rom_settings(id);"
        "CREATE TABLE sram(id);CREATE TABLE states(id);CREATE TABLE app_settings(id);CREATE TABLE app_controllers(id);");}
    const auto original=bytes(previousSchema);
    auto old=RomDatabase::open(previousSchema);
    require(std::holds_alternative<DatabaseError>(old) && std::get<DatabaseError>(old).kind==DatabaseErrorKind::UnsupportedVersion,
        "version 1 without cheats accepted");
    require(bytes(previousSchema)==original,"old version 1 database changed");
    for(int version:{-1,1,2,3,4,5,6}) {
        auto path=fresh(dir,version<0?L"old.sqlite3":L"future.sqlite3");
        {auto db=raw(path);sql(db.get(),("PRAGMA user_version="+std::to_string(version)).c_str());sql(db.get(),"CREATE TABLE preserved(value);INSERT INTO preserved VALUES(71)");}
        const auto previous=bytes(path);auto result=RomDatabase::open(path);
        require(std::holds_alternative<DatabaseError>(result) && std::get<DatabaseError>(result).kind==DatabaseErrorKind::UnsupportedVersion,"unknown version accepted");
        require(bytes(path)==previous,"unknown DB changed");
    }
    const auto path=fresh(dir,L"unversioned.sqlite3");
    {auto db=raw(path);sql(db.get(),"CREATE TABLE preserved(value);INSERT INTO preserved VALUES(89)");}
    const auto previous=bytes(path);auto result=RomDatabase::open(path);
    require(std::holds_alternative<DatabaseError>(result) && std::get<DatabaseError>(result).kind==DatabaseErrorKind::NotEmpty,"nonempty version zero accepted");
    require(bytes(path)==previous,"nonempty DB changed");
    const auto similar=fresh(dir,L"similar-name.sqlite3");
    {auto db=raw(similar);sql(db.get(),"CREATE TABLE sqliteXkeep(value);INSERT INTO sqliteXkeep VALUES(31)");}
    const auto similarBytes=bytes(similar);auto similarResult=RomDatabase::open(similar);
    require(std::holds_alternative<DatabaseError>(similarResult) && std::get<DatabaseError>(similarResult).kind==DatabaseErrorKind::NotEmpty,"sqlite-like user table treated as empty");
    require(bytes(similar)==similarBytes,"sqlite-like user table changed");
}
void constraints(const std::filesystem::path& dir) {
    auto db=open(fresh(dir,L"constraints.sqlite3"));
    require(!db.execute("CREATE TABLE parent(id INTEGER PRIMARY KEY);CREATE TABLE child(parent_id REFERENCES parent(id));"),"tables");
    auto error=db.execute("INSERT INTO child VALUES(5)");
    require(error && error->kind==DatabaseErrorKind::Sqlite && error->code==SQLITE_CONSTRAINT && !error->message.empty(),"FK failure missing");
    require(scalar(db.nativeHandle(),"SELECT count(*) FROM child")==0,"orphan row");
    require(!db.execute("BEGIN;INSERT INTO parent VALUES(5);INSERT INTO child VALUES(5);COMMIT;"),"transaction success");
    require(!db.execute("BEGIN;INSERT INTO parent VALUES(9)"),"begin rollback");
    error=db.execute("INVALID SQL");require(error && error->code==SQLITE_ERROR,"SQL error missing");
    require(!db.execute("ROLLBACK"),"rollback");
    require(scalar(db.nativeHandle(),"SELECT count(*) FROM parent")==1,"rollback lost/kept wrong rows");
}
void failures(const std::filesystem::path& dir) {
    auto result=RomDatabase::open(dir/L"missing-parent"/L"db.sqlite3");
    require(std::holds_alternative<DatabaseError>(result) && std::get<DatabaseError>(result).code==SQLITE_CANTOPEN,"open error missing");
    const auto bad=fresh(dir,L"not-a-db.sqlite3");{std::ofstream f(bad,std::ios::binary);f<<"Not a SQLite database; preserve these bytes.";}
    const auto previous=bytes(bad);auto badResult=RomDatabase::open(bad);
    require(std::holds_alternative<DatabaseError>(badResult) && std::get<DatabaseError>(badResult).code==SQLITE_NOTADB,"bad DB accepted");
    require(bytes(bad)==previous,"bad DB changed");
    const auto path=fresh(dir,L"readonly.sqlite3");{auto db=open(path);}
    std::filesystem::permissions(path,std::filesystem::perms::owner_read,std::filesystem::perm_options::replace);
    auto readonlyResult=RomDatabase::open(path);
    std::filesystem::permissions(path,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
    require(std::holds_alternative<DatabaseError>(readonlyResult),"read-only accepted");
    const auto code=std::get<DatabaseError>(readonlyResult).code;
    require(code==SQLITE_READONLY || code==SQLITE_CANTOPEN,"read-only error code");
}
void commit(const std::filesystem::path& dir) {
    const auto path=fresh(dir,L"commit.sqlite3");
    {auto seed=raw(path);sql(seed.get(),"PRAGMA user_version=0");}
    const auto previous=bytes(path);
    auto reader=raw(path);sql(reader.get(),"BEGIN;SELECT * FROM sqlite_schema");
    const auto failed=RomDatabase::open(path);
    require(std::holds_alternative<DatabaseError>(failed) && std::get<DatabaseError>(failed).code==SQLITE_BUSY,"commit failure missing");
    sql(reader.get(),"ROLLBACK");reader.reset();
    require(bytes(path)==previous,"failed init changed bytes");
    {auto check=raw(path);require(scalar(check.get(),"PRAGMA user_version")==0,"failed commit advanced version");}
    {auto db=open(path);require(scalar(db.nativeHandle(),"PRAGMA user_version")==1,"retry after explicit release");}
}
void cheats(const std::filesystem::path& dir) {
    const auto path=fresh(dir,L"cheats.sqlite3");
    const Cheat first{L"\u6b8b\u6a5f",0xc001,SearchWidth::Word,0x1234,true,true};
    const Cheat second{L"Single",0xc002,SearchWidth::Byte,0x56,true,false};
    const std::vector entries{first,second};
    {
        auto db=open(path);
        sql(db.nativeHandle(),"INSERT INTO roms VALUES(1,zeroblob(32),zeroblob(20),0,8192,'one','one.sms'),"
            "(2,zeroblob(32),zeroblob(20),0,16384,'two','two.sms');");
        require(std::get<std::vector<Cheat>>(db.readCheats(1,Machine::Sms)).empty(),"fresh list");
        require(!db.saveCheats(1,Machine::Sms,entries) && !db.saveCheats(1,Machine::GameGear,{&second,1}) &&
            !db.saveCheats(2,Machine::Sms,{&first,1}),"save isolated lists");
        auto overlap=second;overlap.always=true;
        const std::vector invalid{first,overlap};
        require(db.saveCheats(1,Machine::Sms,invalid).has_value(),"overlapping freezes accepted");
        auto outside=first;outside.address=0xc3ff;
        require(db.saveCheats(1,Machine::Sg1000,{&outside,1}).has_value(),"region overrun accepted");
        sql(db.nativeHandle(),"CREATE TRIGGER fail_cheat BEFORE INSERT ON cheats WHEN NEW.position=1 BEGIN SELECT RAISE(ABORT,'cheat test');END");
        const std::vector reversed{second,first};
        require(db.saveCheats(1,Machine::Sms,reversed).has_value(),"partial replacement accepted");
        require(std::get<std::vector<Cheat>>(db.readCheats(1,Machine::Sms))==entries,"old list lost after failure");
    }
    auto db=open(path);
    require(std::get<std::vector<Cheat>>(db.readCheats(1,Machine::Sms))==entries,"ordered reopen");
    require(std::get<std::vector<Cheat>>(db.readCheats(1,Machine::GameGear))==std::vector{second},"machine isolation");
    require(std::get<std::vector<Cheat>>(db.readCheats(2,Machine::Sms))==std::vector{first},"ROM isolation");
    require(!db.saveCheats(1,Machine::Sms,{}) && std::get<std::vector<Cheat>>(db.readCheats(1,Machine::Sms)).empty(),"empty replacement");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"arguments");const std::string_view operation=argv[1];
        const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);
        if(operation=="open") opening(dir);else if(operation=="schema") schema(dir);
        else if(operation=="constraints") constraints(dir);else if(operation=="failures") failures(dir);
        else if(operation=="commit") commit(dir);else if(operation=="cheats") cheats(dir);else throw std::runtime_error("unknown operation");
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
