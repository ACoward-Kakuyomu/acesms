#include "acesms_fm.h"
#include <ymfm_opl.h>
#include <Windows.h>

#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
struct Module {
    HMODULE handle;
    const acesms_fm_api* api{};
    explicit Module(const std::filesystem::path& path) : handle(LoadLibraryW(path.c_str())) {
        if(!handle) throw std::runtime_error("FM plugin DLL missing");
        const auto get=reinterpret_cast<acesms_fm_get_api_fn>(GetProcAddress(handle,"acesms_fm_get_api"));
        if(get && !get(0) && !get(2)) api=get(1);
    }
    ~Module() {FreeLibrary(handle);}
};
struct Instance {
    const acesms_fm_api& api;
    acesms_fm_instance* handle{};
    explicit Instance(const acesms_fm_api& value) : api(value) {
        const acesms_fm_config cfg{sizeof(acesms_fm_config),3579545,72};
        if(api.create(&cfg,&handle) || !handle) throw std::runtime_error("Plugin create failed");
    }
    ~Instance() {api.destroy(handle);}
    void writeIo(uint8_t p,uint8_t v) {if(api.write_io(handle,p,v)) throw std::runtime_error("Plugin write failed");}
    std::vector<int32_t> advance(uint64_t target,uint64_t previous) {
        std::vector<int32_t> out(static_cast<size_t>(target/72-previous/72));uint32_t count=0;
        if(api.advance_to(handle,target,out.data(),static_cast<uint32_t>(out.size()),&count) || count!=out.size()) throw std::runtime_error("Plugin advance failed");
        return out;
    }
};
struct Reference {
    ymfm::ymfm_interface host;
    ymfm::ym2413 chip{host};
    Reference() {chip.reset();}
    void writeIo(uint8_t port,uint8_t value) {chip.write(port,value);}
    int32_t sample() {ymfm::ym2413::output_data out;chip.generate(&out);return out.data[0]+out.data[1];}
};
template<class T> void configure(T& chip,unsigned mode) {
    const auto write=[&](uint8_t reg,uint8_t data){chip.writeIo(0,reg);chip.writeIo(1,data);};
    if(mode==1) {constexpr uint8_t patch[]{0x21,0x21,0,0x18,0xf2,0xf2,4,4};for(uint8_t i=0;i<8;++i) write(i,patch[i]);}
    for(uint8_t ch=0;ch<9;++ch) {write(0x30+ch,mode==1?0:0x10);write(0x10+ch,0x80+ch);write(0x20+ch,0x19);}
    if(mode==2) write(0x0e,0x3f);
}
bool contract(const acesms_fm_api& api) {
    constexpr uint8_t id[]{0x61,0x63,0x65,0x73,0x6d,0x73,0x2d,0x79,0x6d,0x66,0x6d,0,0,0,0,1};
    constexpr uint8_t compatibility[]{0x79,0x6d,0x66,0x6d,0x2d,0x6f,0x70,0x6c,0x6c,0x2d,0x76,0x31,0,0,0,1};
    if(api.abi_version!=1 || api.struct_size!=232 || std::memcmp(api.descriptor.backend_id,id,16)
        || std::memcmp(api.descriptor.compatibility_id,compatibility,16) || api.descriptor.state_version!=1
        || std::strcmp(api.descriptor.revision,"81aec25ccbb98f4873a255f7551ac4dadac59b4a")
        || api.descriptor.master_clock_hz!=3579545 || api.descriptor.native_divider!=72) return false;
    Instance chip(api);uint32_t count=77,size=0;std::array<int32_t,2> out{123,456};
    if(api.advance_to(chip.handle,71,nullptr,0,&count) || count
        || api.advance_to(chip.handle,72,out.data(),0,&count)!=ACESMS_FM_BUFFER_TOO_SMALL || count || out[0]!=123
        || api.advance_to(chip.handle,70,out.data(),2,&count)!=ACESMS_FM_INVALID_ARGUMENT || count
        || api.advance_to(chip.handle,72,nullptr,1,&count)!=ACESMS_FM_INVALID_ARGUMENT || count) return false;
    if(api.advance_to(chip.handle,72,out.data(),2,&count) || count!=1 || out[1]!=456
        || api.write_io(chip.handle,2,0)!=ACESMS_FM_INVALID_ARGUMENT || api.state_size(chip.handle,&size) || size!=327) return false;
    acesms_fm_instance* raw=nullptr;acesms_fm_config bad{sizeof(acesms_fm_config),3579544,72};
    return api.create(&bad,&raw)==ACESMS_FM_UNSUPPORTED_CONFIG && !raw;
}
bool native(const acesms_fm_api& api) {
    for(unsigned mode=0;mode<3;++mode) {
        Instance chip(api);Reference ref;configure(chip,mode);configure(ref,mode);bool audible=false;
        uint64_t previous=0;
        for(uint64_t n=1;n<=10000;++n) {
            if(n==2500) {chip.writeIo(0,0x20);chip.writeIo(1,9);ref.writeIo(0,0x20);ref.writeIo(1,9);}
            const auto samples=chip.advance(n*72,previous);previous=n*72;
            if(samples[0]!=ref.sample()) return false;audible|=samples[0]!=0;
        }
        if(!audible || api.reset(chip.handle)) return false;
        Instance fresh(api);configure(chip,mode);configure(fresh,mode);
        if(chip.advance(4096*72,0)!=fresh.advance(4096*72,0)) return false;
    }
    return true;
}
std::vector<uint8_t> capture(Instance& chip) {
    uint32_t size=0,written=0;if(chip.api.state_size(chip.handle,&size)) throw std::runtime_error("State size failed");
    std::vector<uint8_t> out(size);
    if(chip.api.save_state(chip.handle,out.data(),size,&written) || written!=size) throw std::runtime_error("State save failed");
    return out;
}
bool state(const acesms_fm_api& api) {
    for(bool released:{false,true}) for(unsigned mode=0;mode<3;++mode) for(uint64_t n:{0ull,1ull,17ull,4095ull,4096ull,4097ull,10000ull,20000ull}) {
        Instance saved(api),unsaved(api),restored(api);configure(saved,mode);configure(unsaved,mode);
        uint64_t previousTime=0;
        if(released) {
            if(saved.advance(1000*72,0)!=unsaved.advance(1000*72,0)) return false;
            previousTime=1000*72;
            for(auto* chip:{&saved,&unsaved}) {
                for(uint8_t ch=0;ch<9;++ch) {chip->writeIo(0,0x20+ch);chip->writeIo(1,9);}
                chip->writeIo(0,0x0e);chip->writeIo(1,0x20);
            }
        }
        const auto time=(n+(released?81000:0))*72+13;
        if(saved.advance(time,previousTime)!=unsaved.advance(time,previousTime)) return false;
        saved.writeIo(0,3);saved.writeIo(1,0);unsaved.writeIo(0,3);unsaved.writeIo(1,0);
        const auto bytes=capture(saved);
        const acesms_fm_load_context context{sizeof(acesms_fm_load_context),3579545,time,72};
        auto bad=bytes;bad[14]=0;
        const auto before=capture(restored);
        if(api.load_state(restored.handle,bad.data(),static_cast<uint32_t>(bad.size()),&context)!=ACESMS_FM_INVALID_STATE
            || capture(restored)!=before) return false;
        const auto reject=[&](size_t offset,uint32_t value,unsigned width) {
            auto corrupt=bytes;for(unsigned i=0;i<width;++i) corrupt[offset+i]=static_cast<uint8_t>(value>>(i*8));
            return api.load_state(restored.handle,corrupt.data(),327,&context)==ACESMS_FM_INVALID_STATE && capture(restored)==before;
        };
        if(!reject(9,1,4) || !reject(13,1,1) || !reject(15,1,1) || !reject(16,1,1) || !reject(17,1,1)
            || !reject(18,1,1) || !reject(20,13440,2) || !reject(24,53,1) || !reject(25,0,4)) return false;
        for(unsigned op=0;op<18;++op) {
            const auto start=147+op*10;
            if(!reject(start+4,1024,2) || !reject(start+6,5,1) || !reject(start+7,1,1)
                || !reject(start+8,2,1) || !reject(start+9,4,1)) return false;
        }
        for(uint32_t length=0;length<327;++length)
            if(api.load_state(restored.handle,bytes.data(),length,&context)!=ACESMS_FM_INVALID_STATE) return false;
        if(api.load_state(restored.handle,bytes.data(),327,&context)) return false;
        uint64_t previous=time;
        for(unsigned step=0;step<5;++step) {
            const auto target=previous+4096*72+1;
            const auto a=saved.advance(target,previous),b=unsaved.advance(target,previous),c=restored.advance(target,previous);
            if(a!=b || a!=c) {std::cerr<<"capture/replay difference mode="<<mode<<" n="<<n<<" step="<<step<<'\n';return false;}
            for(auto* chip:{&saved,&unsaved,&restored}) {chip->writeIo(0,0x20);chip->writeIo(1,step%2?0x19:9);}
            previous=target;
        }
    }
    return true;
}
bool writeFile(const std::filesystem::path& path,std::span<const uint8_t> data) {
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(data.data()),data.size());return bool(out);
}
std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
bool save(const acesms_fm_api& api,const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    for(unsigned mode=0;mode<3;++mode) {
        Instance chip(api);configure(chip,mode);chip.advance(140003,0);
        chip.writeIo(0,7);const auto bytes=capture(chip);
        const auto out=chip.advance((140003ull/72+4096)*72+17,140003);std::vector<uint8_t> pcm;
        for(auto x:out) {auto bits=std::bit_cast<uint32_t>(x);for(unsigned i=0;i<4;++i) pcm.push_back(static_cast<uint8_t>(bits>>(i*8)));}
        if(!writeFile(root/(std::to_string(mode)+".bin"),bytes) || !writeFile(root/(std::to_string(mode)+".pcm"),pcm)) return false;
    }
    return true;
}
bool load(const acesms_fm_api& api,const std::filesystem::path& root) {
    for(unsigned mode=0;mode<3;++mode) {
        Instance chip(api);const auto bytes=readFile(root/(std::to_string(mode)+".bin")),pcm=readFile(root/(std::to_string(mode)+".pcm"));
        const acesms_fm_load_context context{sizeof(acesms_fm_load_context),3579545,140003,72};
        if(api.load_state(chip.handle,bytes.data(),static_cast<uint32_t>(bytes.size()),&context) || pcm.size()!=4096*4) return false;
        const auto out=chip.advance((140003ull/72+4096)*72+17,140003);bool audible=false;
        for(size_t i=0;i<out.size();++i) {uint32_t bits=0;for(unsigned j=0;j<4;++j) bits|=uint32_t(pcm[i*4+j])<<(8*j);if(std::bit_cast<uint32_t>(out[i])!=bits) return false;audible|=out[i]!=0;}
        if(!audible) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=4) return 1;Module module(argv[2]);if(!module.api) return 1;
        const std::string_view name(argv[1]);
        const bool pass=name=="contract"?contract(*module.api):name=="native"?native(*module.api):name=="state"?state(*module.api)
            :name=="save"?save(*module.api,argv[3]):name=="load"&&load(*module.api,argv[3]);
        if(!pass) std::cerr<<"FM plugin test failed: "<<name<<'\n';return pass?0:1;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
