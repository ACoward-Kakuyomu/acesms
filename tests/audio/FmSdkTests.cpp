#include "core/audio/BuiltinFm.h"
#include "core/audio/Emu2413Native.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>
#include <vector>

namespace { bool failNextAllocation=false; }
void* operator new(std::size_t bytes) {
    if(failNextAllocation) {failNextAllocation=false;throw std::bad_alloc();}
    if(auto* p=std::malloc(bytes?bytes:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}

namespace {
using namespace smsgg;
using Handle=std::unique_ptr<acesms_fm_instance,decltype(acesms_fm_api::destroy)>;
constexpr acesms_fm_config config{sizeof(acesms_fm_config),3579545,72};
Handle create(const acesms_fm_api& api) {
    acesms_fm_instance* raw=nullptr;
    if(api.create(&config,&raw)!=ACESMS_FM_OK) return {nullptr,api.destroy};
    return {raw,api.destroy};
}
bool write(const acesms_fm_api& api,acesms_fm_instance* h,uint8_t reg,uint8_t value) {
    return api.write_io(h,0,reg)==0 && api.write_io(h,1,value)==0;
}
bool arguments() {
    static_assert(sizeof(acesms_fm_descriptor)==156 && sizeof(acesms_fm_api)==232);
    static_assert(sizeof(acesms_fm_config)==12 && sizeof(acesms_fm_load_context)==24);
    const auto* api=builtinFmApi(1);
    constexpr uint8_t id[]{0x61,0x63,0x65,0x73,0x6d,0x73,0x2d,0x65,0x6d,0x75,0x32,0x34,0x31,0x33,0,1};
    constexpr uint8_t compatibility[]{0x65,0x6d,0x75,0x32,0x34,0x31,0x33,0x2d,0x6e,0x37,0x32,0x2d,0x76,0x31,0,1};
    if(!api || builtinFmApi(0) || builtinFmApi(2) || api->abi_version!=1 || api->struct_size!=232
        || std::memcmp(api->descriptor.backend_id,id,16) || std::memcmp(api->descriptor.compatibility_id,compatibility,16) || api->descriptor.state_version!=1
        || api->descriptor.master_clock_hz!=3579545 || api->descriptor.native_divider!=72
        || std::strcmp(api->descriptor.revision,"a1d5120903b75f93d3eb7690e34a29c0b4add903")) return false;
    acesms_fm_instance* raw=reinterpret_cast<acesms_fm_instance*>(uintptr_t{1});
    if(api->create(nullptr,&raw)!=ACESMS_FM_INVALID_ARGUMENT || raw || api->create(&config,nullptr)!=ACESMS_FM_INVALID_ARGUMENT) return false;
    auto bad=config;bad.struct_size=11;
    if(api->create(&bad,&raw)!=ACESMS_FM_INVALID_ARGUMENT || raw) return false;
    bad=config;bad.master_clock_hz=3579544;
    if(api->create(&bad,&raw)!=ACESMS_FM_UNSUPPORTED_CONFIG || raw) return false;
    bad=config;bad.native_divider=71;
    if(api->create(&bad,&raw)!=ACESMS_FM_UNSUPPORTED_CONFIG || raw) return false;
    auto h=create(*api);if(!h) return false;
    api->destroy(nullptr);
    uint32_t count=99;
    if(api->reset(nullptr)!=ACESMS_FM_INVALID_ARGUMENT || api->state_size(nullptr,&count)!=ACESMS_FM_INVALID_ARGUMENT || count
        || api->state_size(h.get(),nullptr)!=ACESMS_FM_INVALID_ARGUMENT || api->write_io(h.get(),2,0)!=ACESMS_FM_INVALID_ARGUMENT
        || api->write_io(nullptr,0,0)!=ACESMS_FM_INVALID_ARGUMENT) return false;
    if(api->advance_to(h.get(),71,nullptr,0,&count)!=0 || count) return false;
    std::array<int32_t,2> out{12345,23456};count=99;
    if(api->advance_to(h.get(),72,out.data(),0,&count)!=ACESMS_FM_BUFFER_TOO_SMALL || count || out[0]!=12345
        || api->advance_to(h.get(),70,out.data(),2,&count)!=ACESMS_FM_INVALID_ARGUMENT || count
        || api->advance_to(h.get(),72,nullptr,1,&count)!=ACESMS_FM_INVALID_ARGUMENT || count
        || api->advance_to(h.get(),72,out.data(),2,nullptr)!=ACESMS_FM_INVALID_ARGUMENT) return false;
    if(api->advance_to(h.get(),UINT64_MAX,out.data(),UINT32_MAX,&count)!=ACESMS_FM_BUFFER_TOO_SMALL || count || out[0]!=12345) return false;
    if(api->advance_to(h.get(),72,out.data(),2,&count)!=0 || count!=1 || out[0]!=0 || out[1]!=23456) return false;
    return api->advance_to(h.get(),72,nullptr,0,&count)==0 && count==0;
}
bool native() {
    const auto* api=builtinFmApi(1);
    for(unsigned mode=0;mode<3;++mode) {
        auto h=create(*api);if(!h) return false;
        Emu2413Native expected;
        const auto both=[&](uint8_t r,uint8_t v) {
            expected.writeIo(0,r);expected.writeIo(1,v);return write(*api,h.get(),r,v);
        };
        if(mode==1) {
            constexpr uint8_t patch[]{0x21,0x21,0,0x18,0xf2,0xf2,4,4};
            for(uint8_t i=0;i<8;++i) if(!both(i,patch[i])) return false;
        }
        if(mode==2) {
            for(uint8_t ch=6;ch<9;++ch) if(!both(0x10+ch,0x80) || !both(0x20+ch,9) || !both(0x30+ch,0)) return false;
            if(!both(0x0e,0x3f)) return false;
        } else if(!both(0x30,mode==1?0:0x10) || !both(0x10,0x80) || !both(0x20,0x19)) return false;
        bool audible=false;
        for(uint64_t n=1;n<=5000;++n) {
            if(n==1250 && !both(mode==2?0x0e:0x20,mode==2?0x20:9)) return false;
            std::array<int32_t,1> actual{},reference{};uint32_t count=99;
            if(api->advance_to(h.get(),n*72-1,nullptr,0,&count)!=0 || count) return false;
            if(api->advance_to(h.get(),n*72,actual.data(),1,&count)!=0 || count!=1
                || expected.advanceTo(n*72,reference)!=1 || actual!=reference) return false;
            audible|=actual[0]!=0;
        }
        if(!audible || api->reset(h.get())!=0) return false;
        expected.reset();std::array<int32_t,256> a{},b{};uint32_t count=0;
        if(api->advance_to(h.get(),256*72,a.data(),256,&count)!=0 || count!=256 || expected.advanceTo(256*72,b)!=256 || a!=b) return false;
    }
    return true;
}
bool state() {
    const auto* api=builtinFmApi(1);auto source=create(*api),dest=create(*api);if(!source || !dest) return false;
    if(!write(*api,source.get(),0x30,0x10) || !write(*api,source.get(),0x10,0x80) || !write(*api,source.get(),0x20,0x19)) return false;
    std::vector<int32_t> scratch(2000);uint32_t count=0,size=0,written=0;
    if(api->advance_to(source.get(),140003,scratch.data(),2000,&count)!=0 || count!=1944
        || api->state_size(source.get(),&size)!=0 || size!=3193) return false;
    std::vector<uint8_t> bytes(size),before(size),after(size),sentinel(size,0xa5);
    if(api->save_state(source.get(),bytes.data(),size,&written)!=0 || written!=size
        || api->save_state(dest.get(),before.data(),size,&written)!=0 || written!=size) return false;
    if(api->save_state(dest.get(),sentinel.data(),size-1,&written)!=ACESMS_FM_BUFFER_TOO_SMALL || written
        || api->save_state(dest.get(),nullptr,size,&written)!=ACESMS_FM_INVALID_ARGUMENT || written
        || api->save_state(dest.get(),sentinel.data(),size,nullptr)!=ACESMS_FM_INVALID_ARGUMENT) return false;
    for(auto v:sentinel) if(v!=0xa5) return false;
    acesms_fm_load_context context{sizeof(acesms_fm_load_context),3579545,140003,72};
    auto badContext=context;badContext.master_clock_hz=1;
    if(api->load_state(dest.get(),bytes.data(),size,&badContext)!=ACESMS_FM_UNSUPPORTED_CONFIG) return false;
    badContext=context;badContext.native_divider=71;
    if(api->load_state(dest.get(),bytes.data(),size,&badContext)!=ACESMS_FM_UNSUPPORTED_CONFIG) return false;
    badContext=context;badContext.struct_size=0;
    if(api->load_state(dest.get(),bytes.data(),size,&badContext)!=ACESMS_FM_INVALID_ARGUMENT) return false;
    badContext=context;++badContext.master_ticks;
    if(api->load_state(dest.get(),bytes.data(),size,&badContext)!=ACESMS_FM_INVALID_STATE
        || api->load_state(dest.get(),bytes.data(),size-1,&context)!=ACESMS_FM_INVALID_STATE
        || api->load_state(dest.get(),nullptr,size,&context)!=ACESMS_FM_INVALID_ARGUMENT
        || api->load_state(dest.get(),bytes.data(),size,nullptr)!=ACESMS_FM_INVALID_ARGUMENT) return false;
    auto corrupt=bytes;corrupt[140]=2;
    if(api->load_state(dest.get(),corrupt.data(),size,&context)!=ACESMS_FM_INVALID_STATE
        || api->save_state(dest.get(),after.data(),size,&written)!=0 || before!=after) return false;
    if(api->load_state(dest.get(),bytes.data(),size,&context)!=0) return false;
    std::array<int32_t,4096> a{},b{};uint32_t n=0,m=0;
    const auto target=(140003ull/72+4096)*72+17;
    return api->advance_to(source.get(),target,a.data(),4096,&n)==0 && api->advance_to(dest.get(),target,b.data(),4096,&m)==0
        && n==4096 && m==n && a==b;
}
bool allocation() {
    const auto* api=builtinFmApi(1);acesms_fm_instance* h=reinterpret_cast<acesms_fm_instance*>(uintptr_t{1});
    failNextAllocation=true;
    const auto result=api->create(&config,&h);
    if(result!=ACESMS_FM_OUT_OF_MEMORY || h || failNextAllocation) return false;
    auto recovered=create(*api);
    return bool(recovered);
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="arguments"?arguments():name=="native"?native():name=="state"?state():name=="allocation"&&allocation();
    if(!pass) std::cerr<<"FM SDK failed: "<<name<<'\n';
    return pass?0:1;
}
