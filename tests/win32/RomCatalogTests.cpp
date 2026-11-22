#include "win32/RomDatabase.h"
#include "core/cartridge/RomLoader.h"
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using namespace smsgg;
using namespace smsgg::win32;
void require(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
RomImage image(uint8_t tag=0,size_t size=8192,bool collision=false) {
    std::vector<uint8_t> bytes(size,tag);
    if(collision) {
        for(size_t i=0;i<size-4;++i) bytes[i]=static_cast<uint8_t>(i);
        std::copy_n(std::array<uint8_t,4>{0x33,0x34,0xb6,0xb0}.begin(),4,bytes.end()-4);
    }
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::Auto}));
}
RomIdentity identity(const RomImage& rom) {
    auto result=identifyRom(rom);require(std::holds_alternative<RomIdentity>(result),"hash failed");return std::get<RomIdentity>(result);
}
RomDatabase database(const std::filesystem::path& dir) {
    auto path=dir/L"catalog.sqlite3";std::filesystem::remove(path);
    auto result=RomDatabase::open(path);require(std::holds_alternative<RomDatabase>(result),"open catalog");return std::get<RomDatabase>(std::move(result));
}
RomRecord record(RomDatabase& db,const RomIdentity& id,const RomSettings& settings={},std::wstring_view name=L"name",const std::filesystem::path& path=L"first.gg") {
    auto result=db.registerRom(id,settings,name,path);
    if(auto* e=std::get_if<DatabaseError>(&result)) throw std::runtime_error(e->message);
    return std::get<RomRecord>(std::move(result));
}
std::optional<RomRecord> find(RomDatabase& db,const RomIdentity& id) {
    auto result=db.findRom(id);
    if(auto* e=std::get_if<DatabaseError>(&result)) throw std::runtime_error(e->message);
    return std::get<std::optional<RomRecord>>(std::move(result));
}
int count(RomDatabase& db,const char* sql) {
    sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db.nativeHandle(),sql,-1,&s,nullptr)==SQLITE_OK,"prepare count");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> owner(s,sqlite3_finalize);
    require(sqlite3_step(s)==SQLITE_ROW,"count row");return sqlite3_column_int(s,0);
}
template<size_t N> std::string hex(const std::array<uint8_t,N>& bytes) {
    constexpr char digits[]="0123456789abcdef";std::string out;
    for(auto b:bytes){out+=digits[b>>4];out+=digits[b&15];}return out;
}
void hashes(const std::filesystem::path&) {
    auto zero=image();const auto a=identity(zero);
    require(a.bytes==8192 && a.crc32==0xd8f49994,"CRC/size vector");
    require(hex(a.sha256)=="9f1dcbc35c350d6027f98be0f5c8b43b42ca52b7604459c0c42be3aa88913d47","SHA256 vector");
    require(hex(a.sha1)=="0631457264ff7f8d5fb1edc2c0211992a67c73e6","SHA1 vector");
    const auto b=identity(image(0,8192,true));
    require(hex(b.sha256)=="aafcd8a98e749d87b3601a5b72b62233b2d06a25c4e0835ba00510688d6e2419" && hex(b.sha1)=="762c29bd9cdf5272862c42cf76ff623522376bc1","second independent vector");
    require(a.crc32==b.crc32 && a.sha256!=b.sha256,"actual CRC collision");
    std::vector<uint8_t> prefixed(512,0xa5);prefixed.insert(prefixed.end(),zero.bytes().begin(),zero.bytes().end());
    auto stripped=std::get<RomImage>(RomLoader::fromBytes(std::move(prefixed),{MapperKind::Sega,0,CopierHeader::Strip512}));
    require(identity(stripped)==a && std::all_of(zero.bytes().begin(),zero.bytes().end(),[](auto v){return v==0;}),"normalized payload identity/immutability");
}
void identities(const std::filesystem::path& dir) {
    auto db=database(dir);const auto a=identity(image()),b=identity(image(0,8192,true));
    require(!find(db,a),"unknown ROM");auto first=record(db,a);
    RomSettings changed;changed.ramBytes=32768;
    auto repeated=record(db,a,changed,L"renamed",dir/L"moved"/L"renamed.gg");
    require(repeated==first && count(db,"SELECT count(*) FROM roms")==1,"duplicate changed saved settings/metadata");
    auto second=record(db,b);require(first.id!=second.id && find(db,a)==first && find(db,b)==second,"CRC collision merged ROMs");
    auto hint=a;hint.sha256[0]^=0x80;require(!find(db,hint),"SHA1/CRC-only lookup");
    hint=a;hint.bytes=16384;require(!find(db,hint),"length omitted from key");
    require(!find(db,identity(image(1))),"patched payload merged");
    const auto path=dir/L"renamed.gg";
    {std::ofstream file(path,std::ios::binary);std::vector<char> zeros(8192);file.write(zeros.data(),static_cast<std::streamsize>(zeros.size()));}
    auto fromPath=std::get<RomImage>(RomLoader::load(path,{MapperKind::Auto}));
    require(find(db,identity(fromPath))==first,"path affected identity");
}
void settings(const std::filesystem::path& dir) {
    auto db=database(dir);
    std::array<RomSettings,7> cases{};
    cases[1]={Machine::Sms,VideoStandard::Pal,MapperKind::Sega,32768,false,L"\u97f3\u6e90.dll",FmInterpolation::Linear};
    cases[2]={Machine::Sms,VideoStandard::Ntsc,MapperKind::RomOnly,0,true,{},FmInterpolation::ZohLowPass};
    cases[3]={Machine::GameGear,VideoStandard::Ntsc,MapperKind::Codemasters,8192,false,{},FmInterpolation::Sinc};
    cases[4]={Machine::GameGear,VideoStandard::Ntsc,MapperKind::Sega,16384,false,{},FmInterpolation::Linear};
    cases[5]={Machine::Sms,VideoStandard::Ntsc,MapperKind::Codemasters,0,true,L"\u65e5\u672c'plugin.dll",FmInterpolation::Sinc};
    cases[6]={Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,8192,true,L"ymfm.DLL",FmInterpolation::ZohLowPass};
    for(size_t i=0;i<cases.size();++i) {
        const auto id=identity(image(static_cast<uint8_t>(i),32768));
        const std::wstring name=L"\u8a2d\u5b9a'); DROP TABLE roms; --";
        const auto path=dir/L"\u65e5\u672c\u8a9e ' path.gg";
        const auto saved=record(db,id,cases[i],name,path);
        require(saved.identity==id && saved.settings==cases[i] && saved.name==name && saved.lastPath==path,"saved fields");
        auto result=RomDatabase::open(dir/L"catalog.sqlite3");require(std::holds_alternative<RomDatabase>(result),"reopen");
        require(find(std::get<RomDatabase>(result),id)==saved,"reopened full fields");
    }
    const auto empty=record(db,identity(image(7,32768)),{},std::wstring_view{},{});
    require(empty.name.empty() && empty.lastPath.empty() && find(db,empty.identity)==empty,"empty metadata");
    require(count(db,"SELECT count(*) FROM roms")==8 && count(db,"SELECT count(*) FROM rom_settings")==8,"row pairing");
}
void validation(const std::filesystem::path& dir) {
    auto db=database(dir);const auto id=identity(image());
    std::vector<RomSettings> bad;
    auto add=[&](auto change){RomSettings s;change(s);bad.push_back(std::move(s));};
    add([](auto& s){s.machine=static_cast<Machine>(9);});add([](auto& s){s.video=static_cast<VideoStandard>(9);});
    add([](auto& s){s.mapper=MapperKind::Auto;});add([](auto& s){s.ramBytes=1;});
    add([](auto& s){s.machine=Machine::GameGear;s.video=VideoStandard::Pal;});
    add([](auto& s){s.machine=Machine::GameGear;s.fmEnabled=true;});add([](auto& s){s.video=VideoStandard::Pal;s.fmEnabled=true;});
    add([](auto& s){s.mapper=MapperKind::RomOnly;s.ramBytes=8192;});
    add([](auto& s){s.mapper=MapperKind::Codemasters;});
    add([](auto& s){s.fmPlugin=L"../outside.dll";});add([](auto& s){s.fmPlugin=L"plugin.txt";});
    add([](auto& s){s.interpolation=static_cast<FmInterpolation>(9);});
    for(const auto& s:bad) {
        const auto result=db.registerRom(id,s,L"bad",L"ignored.gg");
        require(std::holds_alternative<DatabaseError>(result) && std::get<DatabaseError>(result).kind==DatabaseErrorKind::InvalidSettings,"invalid settings accepted");
    }
    require(count(db,"SELECT count(*) FROM roms")==0,"invalid request wrote rows");
    for(uint32_t ram:{16384u,32768u}) {
        RomSettings cm;cm.mapper=MapperKind::Codemasters;cm.ramBytes=ram;
        require(std::holds_alternative<DatabaseError>(db.registerRom(identity(image(0,32768)),cm,L"bad",{})),"invalid CM RAM");
    }
    const auto nul=db.registerRom(id,{},std::wstring(L"a\0b",3),L"path.gg");require(std::holds_alternative<DatabaseError>(nul),"embedded NUL accepted");
    record(db,id);
    for(const char* sql:{"INSERT INTO roms(sha256,sha1,crc32,rom_bytes,name,last_path) SELECT sha256,sha1,crc32,rom_bytes,name,last_path FROM roms",
        "INSERT INTO rom_settings SELECT 999,machine,video,mapper,ram_bytes,fm_enabled,fm_plugin,interpolation FROM rom_settings",
        "UPDATE roms SET sha1=X'00'"}) {
        const auto error=db.execute(sql);require(error && error->code==SQLITE_CONSTRAINT,"SQL schema constraint missing");
    }
    for(const char* sql:{"UPDATE rom_settings SET machine=9","UPDATE rom_settings SET interpolation=256","UPDATE rom_settings SET fm_enabled=2","UPDATE roms SET crc32=-1"}) {
        require(!db.execute(sql),"corrupt fixture");auto found=db.findRom(id);
        require(std::holds_alternative<DatabaseError>(found) && std::get<DatabaseError>(found).kind==DatabaseErrorKind::InvalidData,"bad DB row returned");
        require(!db.execute("UPDATE rom_settings SET machine=0,interpolation=0,fm_enabled=0;UPDATE roms SET crc32=3639908756"),"restore fixture");
    }
    require(!db.execute("PRAGMA ignore_check_constraints=ON;UPDATE roms SET sha1=X'00'"),"bad hash fixture");
    require(std::holds_alternative<DatabaseError>(db.findRom(id)),"short stored hash accepted");
    require(!db.execute("UPDATE roms SET sha1=X'0631457264ff7f8d5fb1edc2c0211992a67c73e6';PRAGMA ignore_check_constraints=OFF"),"restore hash");
    require(!db.execute("DELETE FROM rom_settings"),"missing row fixture");
    require(std::holds_alternative<DatabaseError>(db.findRom(id)),"missing settings treated as unregistered");
}
void atomic(const std::filesystem::path& dir) {
    auto db=database(dir);const auto a=identity(image()),b=identity(image(1));auto first=record(db,a);
    require(!db.execute("CREATE TRIGGER fail_settings BEFORE INSERT ON rom_settings BEGIN SELECT RAISE(ABORT,'fixture'); END"),"trigger");
    auto failed=db.registerRom(b,{},L"second",L"second.gg");
    require(std::holds_alternative<DatabaseError>(failed) && count(db,"SELECT count(*) FROM roms")==1 && count(db,"SELECT count(*) FROM rom_settings")==1 && find(db,a)==first,"partial insert on SQL failure");
    require(!db.execute("DROP TRIGGER fail_settings"),"drop trigger");
    sqlite3* reader=nullptr;require(sqlite3_open16((dir/L"catalog.sqlite3").c_str(),&reader)==SQLITE_OK,"reader");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> owner(reader,sqlite3_close_v2);
    require(sqlite3_exec(reader,"BEGIN;SELECT * FROM roms",nullptr,nullptr,nullptr)==SQLITE_OK,"read lock");
    auto blocked=db.registerRom(b,{},L"second",L"second.gg");
    require(std::holds_alternative<DatabaseError>(blocked) && std::get<DatabaseError>(blocked).code==SQLITE_BUSY,"commit error missing");
    require(sqlite3_exec(reader,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"release lock");
    require(sqlite3_get_autocommit(db.nativeHandle()) && !find(db,b) && find(db,a)==first && count(db,"SELECT count(*) FROM rom_settings")==1,"commit failure partial rows");
    require(record(db,b).id!=first.id,"explicit retry");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"arguments");std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);
        const std::string_view op=argv[1];
        if(op=="hashes") hashes(dir);else if(op=="identity") identities(dir);else if(op=="settings") settings(dir);
        else if(op=="validation") validation(dir);else if(op=="atomic") atomic(dir);else throw std::runtime_error("operation");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
