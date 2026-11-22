#include "core/audio/FmBackend.h"
#include "core/audio/BuiltinFm.h"
#include "core/audio/Emu2413Native.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using namespace smsgg;
static_assert(!std::is_copy_constructible_v<FmBackend>);
static_assert(!std::is_move_assignable_v<FmBackend>);
const auto* original=builtinFmApi(1);
int destroyed=0,released=0;
void ACESMS_FM_CALL trackedDestroy(acesms_fm_instance* h) {original->destroy(h);++destroyed;}
acesms_fm_result ACESMS_FM_CALL failCreate(const acesms_fm_config*,acesms_fm_instance** h) {*h=nullptr;return ACESMS_FM_OUT_OF_MEMORY;}
acesms_fm_result ACESMS_FM_CALL failWrite(acesms_fm_instance*,uint32_t,uint8_t) {return ACESMS_FM_INVALID_ARGUMENT;}
acesms_fm_result ACESMS_FM_CALL badCount(acesms_fm_instance*,uint64_t,int32_t*,uint32_t,uint32_t* count) {*count=0;return ACESMS_FM_OK;}
acesms_fm_result ACESMS_FM_CALL badSave(acesms_fm_instance*,uint8_t*,uint32_t,uint32_t* count) {*count=0;return ACESMS_FM_OK;}
acesms_fm_result ACESMS_FM_CALL failReset(acesms_fm_instance*) {return ACESMS_FM_OUT_OF_MEMORY;}
bool validation() {
    auto good=FmBackendDefinition::fromApi(original);if(!std::holds_alternative<FmBackendPtr>(good)) return false;
    if(!std::holds_alternative<std::string>(FmBackendDefinition::fromApi(nullptr))) return false;
    for(unsigned i=0;i<18;++i) {
        auto api=*original;
        switch(i) {
        case 0:api.abi_version=2;break;case 1:--api.struct_size;break;case 2:api.create=nullptr;break;
        case 3:api.destroy=nullptr;break;case 4:api.reset=nullptr;break;case 5:api.advance_to=nullptr;break;
        case 6:api.write_io=nullptr;break;case 7:api.state_size=nullptr;break;case 8:api.save_state=nullptr;break;
        case 9:api.load_state=nullptr;break;case 10:api.descriptor.native_divider=71;break;
        case 11:api.descriptor.master_clock_hz=1;break;case 12:api.descriptor.state_version=0;break;
        case 13:std::memset(api.descriptor.backend_id,0,16);break;case 14:std::memset(api.descriptor.name,'x',64);break;
        case 15:std::memset(api.descriptor.compatibility_id,0,16);break;
        case 16:api.descriptor.name[0]=0;break;case 17:std::memset(api.descriptor.revision,'x',48);break;
        }
        if(!std::holds_alternative<std::string>(FmBackendDefinition::fromApi(&api))) return false;
    }
    return FmBackendDefinition::builtin()->api().descriptor.state_version==1;
}
bool lifetime() {
    destroyed=released=0;auto api=*original;api.destroy=trackedDestroy;
    std::shared_ptr<void> module(new int(7),[](void* p){delete static_cast<int*>(p);if(destroyed!=2) released=-100;else ++released;});
    auto result=FmBackendDefinition::fromApi(&api,module);auto def=std::get<FmBackendPtr>(std::move(result));module.reset();
    {
        FmBackend a(def,3579545),b(def,3579545);def.reset();
        if(released || destroyed) return false;
        std::array<int32_t,1> out{};if(a.advanceTo(72,out)!=1 || b.advanceTo(72,out)!=1) return false;
    }
    return destroyed==2 && released==1;
}
bool native() {
    FmBackend chip(FmBackendDefinition::builtin(),3579545);Emu2413Native reference;
    for(auto [reg,data]:std::array<std::array<uint8_t,2>,3>{{{0x30,0x10},{0x10,0x80},{0x20,0x19}}}) {
        chip.writeIo(0,reg);chip.writeIo(1,data);reference.writeIo(0,reg);reference.writeIo(1,data);
    }
    std::array<int32_t,1> a{},b{};
    for(uint64_t n=1;n<=5000;++n) if(chip.advanceTo(n*72,a)!=1 || reference.advanceTo(n*72,b)!=1 || a!=b || chip.masterTicks()!=n*72) return false;
    chip.reset();reference.reset();
    return chip.masterTicks()==0 && chip.advanceTo(72,a)==1 && reference.advanceTo(72,b)==1 && a==b;
}
bool failures() {
    for(unsigned i=0;i<5;++i) {
        auto api=*original;
        switch(i) {case 0:api.create=failCreate;break;case 1:api.write_io=failWrite;break;case 2:api.advance_to=badCount;break;case 3:api.save_state=badSave;break;case 4:api.reset=failReset;break;}
        auto def=std::get<FmBackendPtr>(FmBackendDefinition::fromApi(&api));
        try {
            FmBackend chip(def,3579545);std::array<int32_t,1> out{};
            if(i==1) chip.writeIo(0,0);if(i==2) chip.advanceTo(72,out);if(i==3) chip.saveState();if(i==4) chip.reset();
            return false;
        } catch(const std::runtime_error&) {}
    }
    return true;
}
bool state() {
    FmBackend a(FmBackendDefinition::builtin(),3579545),b(FmBackendDefinition::builtin(),3579545);
    a.writeIo(0,0x30);a.writeIo(1,0x10);a.writeIo(0,0x10);a.writeIo(1,0x80);a.writeIo(0,0x20);a.writeIo(1,0x19);
    std::array<int32_t,2000> temp{};a.advanceTo(140003,temp);
    const auto bytes=a.saveState(),before=b.saveState();
    if(bytes.size()!=3193 || a.stateSize()!=3193 || b.loadState(bytes,140004)==ACESMS_FM_OK || b.masterTicks()!=0 || b.saveState()!=before) return false;
    if(b.loadState(bytes,140003)!=ACESMS_FM_OK || b.masterTicks()!=140003) return false;
    std::array<int32_t,4096> first{},second{};const auto target=(140003ull/72+4096)*72+17;
    return a.advanceTo(target,first)==4096 && b.advanceTo(target,second)==4096 && first==second;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="validation"?validation():name=="lifetime"?lifetime():name=="native"?native():name=="failures"?failures():name=="state"&&state();
    if(!pass) std::cerr<<"FM backend failed: "<<name<<'\n';return pass?0:1;
}
