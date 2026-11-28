#include "win32/RomDatabase.h"
#include "win32/DbSramStore.h"
#include "win32/EmulationSession.h"
#include "win32/FmPlugin.h"
#include "core/cartridge/RomLoader.h"
#include "../system/Stage8TestRom.h"
#include <sqlite3.h>

#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace smsgg;
using namespace smsgg::win32;
void require(bool ok,const char* why) {if(!ok) throw std::runtime_error(why);}
RomImage image(uint8_t tag=0) {
    auto bytes=test::stage8FmRom();bytes.resize(32768);bytes.back()=tag;
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{MapperKind::Sega,8192}));
}
RomDatabase database(const std::filesystem::path& dir,bool fresh=true) {
    const auto path=dir/L"states.sqlite3";if(fresh) std::filesystem::remove(path);
    auto result=RomDatabase::open(path);require(std::holds_alternative<RomDatabase>(result),"open");return std::get<RomDatabase>(std::move(result));
}
RomRecord add(RomDatabase& db,const RomImage& rom,bool fm=true,FmInterpolation method=FmInterpolation::Sinc) {
    RomSettings settings;settings.ramBytes=8192;settings.fmEnabled=fm;settings.interpolation=method;
    auto result=db.registerRom(std::get<RomIdentity>(identifyRom(rom)),settings,L"fixture",L"unused.sms");
    require(std::holds_alternative<RomRecord>(result),"register");return std::get<RomRecord>(std::move(result));
}
std::unique_ptr<EmulatorSystemState> capture(const EmulatorSystem& system) {
    auto state=std::make_unique<EmulatorSystemState>();system.captureState(*state);return state;
}
void advance(EmulatorSystem& system,unsigned count=2000) {
    for(unsigned n=0;n<count;++n) require(std::holds_alternative<uint32_t>(system.step()),"step");
}
using Statement=std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)>;
Statement statement(sqlite3* db,const char* sql) {
    sqlite3_stmt* s=nullptr;require(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK,"prepare");return Statement(s,sqlite3_finalize);
}
std::vector<uint8_t> blob(RomDatabase& db,int64_t id) {
    auto s=statement(db.nativeHandle(),"SELECT data FROM states WHERE state_id=?1");
    require(sqlite3_bind_int64(s.get(),1,id)==SQLITE_OK && sqlite3_step(s.get())==SQLITE_ROW,"blob row");
    const auto* p=static_cast<const uint8_t*>(sqlite3_column_blob(s.get(),0));const auto n=sqlite3_column_bytes(s.get(),0);
    return n?std::vector<uint8_t>(p,p+n):std::vector<uint8_t>{};
}
int64_t save(RomDatabase& db,int64_t romId,const StateProfile& p,const EmulatorSystemState& state,std::optional<int64_t> id={}) {
    auto result=db.saveState(romId,L"\u65e5\u672c\u8a9e ' ; --",p,state,id);
    require(std::holds_alternative<int64_t>(result),"save");return std::get<int64_t>(result);
}
SavedState read(RomDatabase& db,int64_t romId,int64_t id,const StateProfile& p) {
    auto result=db.readState(romId,id,p);require(std::holds_alternative<SavedState>(result),"read");return std::get<SavedState>(std::move(result));
}
void roundtrip(const std::filesystem::path& dir,const FmBackendPtr& backend) {
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear}) {
        int64_t id{},romId{};std::vector<uint8_t> expected;
        const auto rom=image(static_cast<uint8_t>(method));
        EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,true,method,backend);
        advance(session.system());auto state=capture(session.system());state->cartridge.ram[17]=0x5a;
        {
            auto db=database(dir);const auto record=add(db,rom,true,method);romId=record.id;
            DbSramStore sram(db,record);require(std::holds_alternative<std::vector<uint8_t>>(sram.load()),"SRAM load");
            id=save(db,romId,session.profile(),*state);auto first=read(db,romId,id,session.profile());
            require(first.id==id && first.name==L"\u65e5\u672c\u8a9e ' ; --" && first.createdAt>0 && first.updatedAt>0,"metadata");
            expected=encodeState(session.profile(),*state);require(blob(db,id)==expected,"existing codec bytes");
            require(save(db,romId,session.profile(),*state)!=id,"multiple same name/time");
            state->cartridge.ram[17]=0xa7;
            state->vdp.scrollX=8;state->vdp.nextScrollX=16;
            require(save(db,romId,session.profile(),*state,id)==id,"overwrite ID");
            auto updated=read(db,romId,id,session.profile());require(updated.createdAt==first.createdAt,"creation time preserved");
            expected=encodeState(session.profile(),*state);require(blob(db,id)==expected,"overwrite bytes");
            require(!sram.save(std::vector<uint8_t>(8192)),"baseline unchanged");
            auto disk=sram.load();require(std::get<std::vector<uint8_t>>(disk)==std::vector<uint8_t>(8192),"state save/read leaves SRAM");
        }
        auto db=database(dir,false);auto loaded=read(db,romId,id,session.profile());
        require(encodeState(session.profile(),*loaded.state)==expected,"reopen all values");
        EmulatorSystem candidate(rom,VideoStandard::Ntsc,48000,Machine::Sms,true,method,backend);
        require(candidate.restoreState(*loaded.state)==ACESMS_FM_OK,"backend restore");
        loaded.state->cartridge.ram[17]=0;require(blob(db,id)==expected,"independent ownership");
    }
}
void invalid(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=image();auto record=add(db,rom);
    EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,true);
    advance(session.system());auto state=capture(session.system());const auto current=encodeState(session.profile(),*state);
    const auto id=save(db,record.id,session.profile(),*state);const auto other=add(db,image(1));
    require(std::holds_alternative<DatabaseError>(db.readState(other.id,id,session.profile())),"other ROM ID");
    require(std::holds_alternative<DatabaseError>(db.readState(record.id,id+1,session.profile())),"missing ID");
    auto p=session.profile();p.sampleRate=44100;
    require(std::holds_alternative<StateCodecError>(db.readState(record.id,id,p)),"rate mismatch");
    p=session.profile();p.romCrc32^=1;require(std::holds_alternative<StateCodecError>(db.readState(record.id,id,p)),"ROM codec mismatch");
    p=session.profile();p.fm->method=FmInterpolation::Linear;
    require(!std::holds_alternative<SavedState>(db.readState(record.id,id,p)),"FM method mismatch");
    require(std::holds_alternative<DatabaseError>(db.saveState(record.id,std::wstring_view(L"a\0b",3),session.profile(),*state)),"NUL name");
    require(std::holds_alternative<DatabaseError>(db.saveState(other.id,L"wrong",session.profile(),*state,id)),"cross ROM overwrite");
    const auto empty=db.saveState(record.id,L"",session.profile(),*state);
    require(std::holds_alternative<int64_t>(empty) && read(db,record.id,std::get<int64_t>(empty),session.profile()).name.empty(),"empty name");
    state->audio.fm->backend[0]^=1;
    const auto opaque=save(db,record.id,session.profile(),*state);
    auto decoded=read(db,record.id,opaque,session.profile());
    EmulatorSystem candidate(rom,VideoStandard::Ntsc,48000,Machine::Sms,true);
    require(candidate.restoreState(*decoded.state)!=ACESMS_FM_OK,"opaque backend rejected by candidate");
    state->audio.fm->backend[0]^=1;
    auto bad=blob(db,id);bad.back()^=1;
    auto s=statement(db.nativeHandle(),"UPDATE states SET data=?1 WHERE state_id=?2");
    require(sqlite3_bind_blob(s.get(),1,bad.data(),static_cast<int>(bad.size()),SQLITE_STATIC)==SQLITE_OK && sqlite3_bind_int64(s.get(),2,id)==SQLITE_OK && sqlite3_step(s.get())==SQLITE_DONE,"corrupt");s.reset();
    require(std::holds_alternative<StateCodecError>(db.readState(record.id,id,session.profile())),"CRC corruption");
    require(!db.execute("UPDATE states SET data=x''"),"truncate");require(std::holds_alternative<DatabaseError>(db.readState(record.id,id,session.profile())),"size");
    save(db,record.id,session.profile(),*state,id);require(!db.execute("UPDATE states SET created_at=-1"),"bad metadata");
    require(std::holds_alternative<DatabaseError>(db.readState(record.id,id,session.profile())),"metadata rejected");
    require(encodeState(session.profile(),*capture(session.system()))==current,"current system preserved");
}
void failures(const std::filesystem::path& dir) {
    auto db=database(dir);const auto rom=image();auto record=add(db,rom,false);
    EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000);auto state=capture(session.system());
    const auto id=save(db,record.id,session.profile(),*state);const auto old=blob(db,id);state->cartridge.ram[0]=42;
    require(!db.execute("CREATE TRIGGER fail_update BEFORE UPDATE ON states BEGIN SELECT RAISE(ABORT,'fixture');END"),"trigger");
    require(std::holds_alternative<DatabaseError>(db.saveState(record.id,L"fail",session.profile(),*state,id)),"step failure");
    require(blob(db,id)==old && sqlite3_get_autocommit(db.nativeHandle()),"step rollback");require(!db.execute("DROP TRIGGER fail_update"),"drop trigger");
    sqlite3* raw=nullptr;require(sqlite3_open16((dir/L"states.sqlite3").c_str(),&raw)==SQLITE_OK,"reader");
    std::unique_ptr<sqlite3,decltype(&sqlite3_close_v2)> reader(raw,sqlite3_close_v2);
    require(sqlite3_exec(raw,"BEGIN;SELECT * FROM states",nullptr,nullptr,nullptr)==SQLITE_OK,"lock");
    auto failed=db.saveState(record.id,L"busy",session.profile(),*state,id);
    require(std::holds_alternative<DatabaseError>(failed) && std::get<DatabaseError>(failed).code==SQLITE_BUSY,"commit failure");
    require(sqlite3_exec(raw,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,"unlock");
    require(blob(db,id)==old && sqlite3_get_autocommit(db.nativeHandle()),"commit rollback");
    save(db,record.id,session.profile(),*state,id);
    require(std::holds_alternative<DatabaseError>(db.saveState(999,L"missing",session.profile(),*state)),"FK failure");
    require(!db.execute("PRAGMA query_only=ON"),"query only");
    require(std::holds_alternative<DatabaseError>(db.saveState(record.id,L"readonly",session.profile(),*state)),"write failure");
    require(!db.execute("PRAGMA query_only=OFF;ALTER TABLE states RENAME TO hidden"),"rename");
    require(std::holds_alternative<DatabaseError>(db.readState(record.id,id,session.profile())),"read SQL failure");
}
std::vector<uint8_t> trace(EmulatorSystem& system,const StateProfile& profile) {
    std::vector<uint8_t> bytes;
    for(unsigned n=0;n<20000;++n) {
        advance(system,1);
        for(const auto& frame:system.audioFrames()) for(float sample:{frame.left,frame.right}) {
            const auto value=std::bit_cast<uint32_t>(sample);for(unsigned b=0;b<4;++b) bytes.push_back(static_cast<uint8_t>(value>>(b*8)));
        }
    }
    const auto state=encodeState(profile,*capture(system));bytes.insert(bytes.end(),state.begin(),state.end());return bytes;
}
void process(const std::filesystem::path& dir,bool produce) {
    const auto rom=image();EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,true);
    auto db=database(dir,produce);auto record=add(db,rom);const auto path=dir/L"expected.bin";
    if(produce) {
        advance(session.system(),30000);auto state=capture(session.system());state->cartridge.ram[0]=73;
        require(session.system().restoreState(*state)==ACESMS_FM_OK,"producer RAM");
        require(save(db,record.id,session.profile(),*state)==1,"first ID");
        const auto expected=trace(session.system(),session.profile());std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(expected.data()),expected.size());require(bool(file),"trace write");
    } else {
        auto loaded=read(db,record.id,1,session.profile());require(session.system().restoreState(*loaded.state)==ACESMS_FM_OK,"consumer restore");
        std::ifstream file(path,std::ios::binary);require(bool(file),"trace file");const std::vector<uint8_t> expected{std::istreambuf_iterator<char>(file),{}};
        require(trace(session.system(),session.profile())==expected,"separate process CPU RAM images PCM");
    }
}
}
void slots(const std::filesystem::path& dir) {
    auto rom=image(); EmulationSession session(rom,dir/L"unused.sms",VideoStandard::Ntsc,48000,Machine::Sms,true);
    auto db=database(dir); const auto record=add(db,rom); auto state=capture(session.system());
    for(unsigned slot=1;slot<=10;++slot)
        require(std::holds_alternative<int64_t>(db.saveSlot(record.id,slot,session.profile(),*state)),"save slot");
    const auto first=std::get<std::vector<StateSummary>>(db.listStates(record.id));
    require(first.size()==10 && first[4].slot==5,"fixed slots");
    const auto beforeRename=blob(db,first[4].id);
    require(!db.renameState(record.id,first[4].id,L"branch"),"rename");
    const auto renamed=std::get<std::vector<StateSummary>>(db.listStates(record.id));
    require(renamed[4].updatedAt==first[4].updatedAt && blob(db,first[4].id)==beforeRename,"rename keeps timestamp and BLOB");
    require(std::holds_alternative<int64_t>(db.saveSlot(record.id,5,session.profile(),*state)),"overwrite");
    auto list=std::get<std::vector<StateSummary>>(db.listStates(record.id));
    require(list.size()==10 && list[4].id==first[4].id && list[4].name==L"branch","retain slot and name");
    require(std::holds_alternative<DatabaseError>(db.saveSlot(record.id,11,session.profile(),*state)),"invalid slot");
    require(!db.deleteState(record.id,first[4].id),"delete");
    list=std::get<std::vector<StateSummary>>(db.listStates(record.id));
    require(list.size()==9 && list[4].slot==6,"no renumber");

    const auto legacyPath=dir/L"migration.sqlite3";
    std::filesystem::remove(legacyPath);
    {
        auto legacy=std::get<RomDatabase>(RomDatabase::open(legacyPath));
        const auto rec=add(legacy,rom);
        for(unsigned n=0;n<12;++n) require(std::holds_alternative<int64_t>(
            legacy.saveState(rec.id,L"old",session.profile(),*state)),"legacy save");
        require(!legacy.execute("DROP INDEX state_slots;ALTER TABLE states DROP COLUMN slot;"
            "UPDATE states SET updated_at=1000-state_id;PRAGMA user_version=4"),"version4 fixture");
    }
    auto refused=RomDatabase::open(legacyPath);
    require(std::get<DatabaseError>(refused).kind==DatabaseErrorKind::UnsupportedVersion,"version 4 rejected");
}
int main(int argc,char** argv) {
    try {
        require(argc==3 || argc==4,"arguments");const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);
        FmBackendPtr backend;if(argc==4) {auto path=std::filesystem::absolute(argv[3]);auto result=loadFmPlugin(path.parent_path(),path.filename());require(std::holds_alternative<FmBackendPtr>(result),"plugin");backend=std::get<FmBackendPtr>(std::move(result));}
        const std::string_view op=argv[1];if(op=="slots") slots(dir);else if(op=="roundtrip") roundtrip(dir,backend);else if(op=="invalid") invalid(dir);else if(op=="failures") failures(dir);else if(op=="produce" || op=="consume") process(dir,op=="produce");else require(false,"operation");return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
