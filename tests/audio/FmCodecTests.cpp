#include "core/audio/Emu2413Native.h"

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

namespace {
using namespace smsgg;
void write(Emu2413Native& chip,uint8_t address,uint8_t data) {chip.writeIo(0,address);chip.writeIo(1,data);}
void configure(Emu2413Native& chip,unsigned mode) {
    constexpr uint8_t patch[]{0x71,0x61,0x1e,0x18,0xf2,0xf2,0x04,0x04};
    for(uint8_t i=0;i<8;++i) write(chip,i,patch[i]);
    for(uint8_t ch=0;ch<9;++ch) {write(chip,0x30+ch,mode==1?0:0x10);write(chip,0x10+ch,0x80+ch);write(chip,0x20+ch,0x19);}
    if(mode==2) write(chip,0x0e,0x3f);
}
void advance(Emu2413Native& chip,uint64_t to) {
    std::vector<int32_t> out(static_cast<size_t>(to/72-chip.masterTicks()/72));chip.advanceTo(to,out);
}
bool bytes() {
    static_assert(Emu2413Native::stateBytes==3193);
    Emu2413Native chip,restored;
    auto s=chip.captureState();s.masterTicks=0x0102030405060708ull;s.egCounter=static_cast<uint32_t>(s.masterTicks/72);
    s.address=0xfd;s.amPhase=-7;s.slots[0].output[0]=-1234;
    chip.restoreState(s);
    std::array<uint8_t,3200> out;out.fill(0xa5);chip.saveState(out);
    constexpr uint8_t tick[]{8,7,6,5,4,3,2,1};
    for(size_t i=0;i<8;++i) if(out[i]!=tick[i]) return false;
    if(out[8]!=0 || out[9]!=0xfd || out[10] || out[11] || out[12]) return false;
    if(out[91]!=0xf9 || out[92]!=0xff || out[93]!=0xff || out[94]!=0xff || out[96]!=1) return false;
    if(out[137]!=0 || out[138]!=0 || out[139]!=0 || out[140]!=0
        || out[141]!=0x2e || out[142]!=0xfb || out[143]!=0xff || out[144]!=0xff
        || out[137+26]!=3 || out[137+42]!=127 || out[137+46]!=255) return false;
    if(out[3013]!=3 || out[3013+15]!=0 || out[3029]!=0 || out[3030]!=0x10) return false;
    for(size_t i=3193;i<out.size();++i) if(out[i]!=0xa5) return false;
    if(!restored.loadState(std::span(out).first(3193),s.masterTicks) || restored.captureState()!=s || chip.captureState()!=s) return false;
    return true;
}
bool reject() {
    Emu2413Native source,dest;configure(source,2);advance(source,140003);configure(dest,1);advance(dest,999999);
    std::array<uint8_t,3193> valid{};source.saveState(valid);const auto before=dest.captureState();
    for(size_t length=0;length<valid.size();++length)
        if(dest.loadState(std::span(valid).first(length),140003) || dest.captureState()!=before) return false;
    auto extra=std::vector<uint8_t>(valid.begin(),valid.end());extra.push_back(0);
    if(dest.loadState(extra,140003) || dest.loadState(valid,140004) || dest.captureState()!=before) return false;
    const auto bad=[&](size_t offset,uint32_t value,unsigned width) {
        auto mutated=valid;for(unsigned i=0;i<width;++i) mutated[offset+i]=static_cast<uint8_t>(value>>(8*i));
        return !dest.loadState(mutated,140003) && dest.captureState()==before;
    };
    for(auto [offset,value,width]:std::array<std::array<uint32_t,3>,13>{{
        {8,1,1},{9,256,4},{77,1,1},{78,0x40000,4},{82,2,1},{83,0,4},{95,14,1},
        {96,0,4},{96,0x800000,4},{100,2,1},{101,19,4},{3013,4,1},{3029,4097,4}}}) if(!bad(offset,value,width)) return false;
    if(!bad(3157,0x4000,4) || !bad(101,0xffffffff,4) || !bad(3029,0xffffffff,4)) return false;
    for(size_t slot=0;slot<18;++slot) {
        const auto start=137+slot*50;
        if(!bad(start,18,1) || !bad(start+2,38,1) || !bad(start+3,2,1)) return false;
    }
    for(auto [offset,value,width]:std::array<std::array<uint32_t,3>,21>{{
        {1,1,1},{4,4096,4},{12,0x80000,4},{16,1024,4},{20,2,1},{21,4096,2},{23,512,2},
        {25,8,1},{26,5,1},{27,64,4},{27,1,4},{31,2,1},{32,2,1},{33,239,2},{35,16,1},
        {36,16,1},{37,4,1},{38,32,4},{42,128,4},{46,16,4},{27,0xffffffff,4}}}) if(!bad(137+offset,value,width)) return false;
    constexpr std::array<uint32_t,13> beyond{64,8,2,16,16,16,16,16,2,4,2,2,2};
    for(size_t patch=0;patch<38;++patch) for(size_t field=0;field<13;++field)
        if(!bad(1037+patch*52+field*4,beyond[field],4)) return false;
    return dest.loadState(valid,140003) && dest.captureState()==source.captureState();
}
bool limits() {
    Emu2413Native source,dest;
    std::array<uint8_t,3193> bytes{};
    for(unsigned reg=0;reg<64;++reg) for(unsigned data=0;data<256;++data) {
        write(source,static_cast<uint8_t>(reg),static_cast<uint8_t>(data));
        source.writeIo(0,static_cast<uint8_t>(reg|(data&0xc0))); // Also retain ignored/high address latches.
        source.saveState(bytes);
        if(!dest.loadState(bytes,source.masterTicks()) || dest.captureState()!=source.captureState()) {
            std::cerr<<"Valid state rejected at register="<<reg<<" data="<<data<<'\n';return false;
        }
        std::array<int32_t,4> a{},b{};
        const auto at=source.masterTicks()+(data*17+reg)%145;
        if(source.advanceTo(at,a)!=dest.advanceTo(at,b) || a!=b) return false;
    }
    return true;
}
std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
bool writeFile(const std::filesystem::path& path,std::span<const uint8_t> bytes) {
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());return bool(out);
}
bool save(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    for(unsigned mode=0;mode<3;++mode) {
        Emu2413Native chip;configure(chip,mode);advance(chip,140003);
        write(chip,3,0);chip.writeIo(0,7); // Leave a pending WS/EG update and address latch.
        std::array<uint8_t,3193> state{};chip.saveState(state);
        std::array<int32_t,4096> samples{};
        if(chip.advanceTo((140003ull/72+4096)*72+17,samples)!=4096) return false;
        std::vector<uint8_t> pcm;pcm.reserve(samples.size()*4);
        for(auto sample:samples) {const auto bits=std::bit_cast<uint32_t>(sample);for(unsigned i=0;i<4;++i) pcm.push_back(static_cast<uint8_t>(bits>>(8*i)));}
        if(!writeFile(root/(std::to_string(mode)+".bin"),state) || !writeFile(root/(std::to_string(mode)+".pcm"),pcm)) return false;
    }
    return true;
}
bool load(const std::filesystem::path& root) {
    for(unsigned mode=0;mode<3;++mode) {
        const auto state=readFile(root/(std::to_string(mode)+".bin")),pcm=readFile(root/(std::to_string(mode)+".pcm"));
        Emu2413Native chip;configure(chip,(mode+1)%3);advance(chip,999999);
        if(!chip.loadState(state,140003) || pcm.size()!=4096*4) return false;
        std::array<int32_t,4096> actual{};
        if(chip.advanceTo((140003ull/72+4096)*72+17,actual)!=4096) return false;
        bool audible=false;
        for(size_t i=0;i<actual.size();++i) {
            uint32_t expected=0;for(unsigned byte=0;byte<4;++byte) expected|=uint32_t(pcm[i*4+byte])<<(8*byte);
            if(std::bit_cast<uint32_t>(actual[i])!=expected) return false;
            audible|=actual[i]!=0;
        }
        if(!audible) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc>=2?argv[1]:"";
    const bool pass=name=="bytes"?bytes():name=="reject"?reject():name=="limits"?limits():argc==3 && (name=="save"?save(argv[2]):name=="load"&&load(argv[2]));
    if(!pass) std::cerr<<"FM codec failed: "<<name<<'\n';
    return pass?0:1;
}
