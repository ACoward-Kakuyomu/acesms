#include "win32/FmPlugin.h"
#include "core/audio/AudioMixer.h"
#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;
bool contract(const std::filesystem::path& dll,const std::filesystem::path& scratch) {
    const auto previousMode=GetThreadErrorMode();
    std::filesystem::create_directories(scratch);
    const auto bad=scratch/"invalid.dll";
    {std::ofstream file(bad,std::ios::binary);file<<"invalid DLL fixture";if(!file) return false;}
    for(const auto& name:{std::filesystem::path{},std::filesystem::path(L"../outside.dll"),dll,dll.filename().replace_extension(L".txt")})
        if(!std::holds_alternative<std::string>(loadFmPlugin(dll.parent_path(),name))) return false;
    if(!std::holds_alternative<std::string>(loadFmPlugin(dll.parent_path(),L"missing.dll")) ||
       !std::holds_alternative<std::string>(loadFmPlugin(scratch,bad.filename()))) return false;
    auto loaded=loadFmPlugin(dll.parent_path(),dll.filename());
    std::array<wchar_t,MAX_PATH> system{};
    if(!GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size())) ||
       !std::holds_alternative<std::string>(loadFmPlugin(system.data(),L"kernel32.dll"))) return false;
    return std::holds_alternative<FmBackendPtr>(loaded) && std::get<FmBackendPtr>(loaded)->api().descriptor.state_version==1 && GetThreadErrorMode()==previousMode;
}
bool lifetime(const std::filesystem::path& dll) {
    {
        auto result=loadFmPlugin(dll.parent_path(),dll.filename());
        auto definition=std::get<FmBackendPtr>(std::move(result));
        FmBackend first(definition,3579545),second(definition,3579545);definition.reset();
        if(!GetModuleHandleW(dll.c_str())) return false;
        first.writeIo(0,0x30);first.writeIo(1,0x10);
        std::array<int32_t,1> sample{};first.advanceTo(73,sample);
        if(second.loadState(first.saveState(),73)!=ACESMS_FM_OK) return false;
        first.advanceTo(144,sample);second.advanceTo(144,sample);
    }
    return !GetModuleHandleW(dll.c_str());
}
bool mixer(const std::filesystem::path& dll) {
    const auto definition=std::get<FmBackendPtr>(loadFmPlugin(dll.parent_path(),dll.filename()));
    for(const auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear})
    for(const uint32_t rate:{44100u,48000u,96000u}) {
        AudioMixer actual(3579545,rate,true,method,definition);
        FmBackend reference(definition,3579545);FmResampler converter(3579545,rate,method);
        actual.writeFmControl(1);
        for(auto [reg,value]:std::array<std::array<uint8_t,2>,3>{{{0x30,0x10},{0x10,0x80},{0x20,0x19}}}) {
            actual.writeFm(0,reg);actual.writeFm(1,value);reference.writeIo(0,reg);reference.writeIo(1,value);
        }
        bool audible=false;
        for(uint64_t n=1;n<=4096;++n) {
            const auto time=(n*3579545+rate-1)/rate;
            std::array<int32_t,3> native{};const auto count=reference.advanceTo(time,native);
            for(size_t i=0;i<count;++i) converter.push(native[i]);
            std::array<AudioFrame,1> output{};
            const auto expected=static_cast<float>(converter.sample()/32768*.5);
            if(actual.advanceTo(time,output)!=1 || output[0].left!=expected || output[0].right!=expected) return false;
            audible|=expected!=0;
        }
        if(!audible) return false;
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc!=4) return 1;
    const auto dll=std::filesystem::absolute(argv[2]);const std::string_view operation=argv[1];
    const bool passed=operation=="contract"?contract(dll,argv[3]):operation=="lifetime"?lifetime(dll):operation=="mixer"&&mixer(dll);
    if(!passed) std::cerr<<"FM loader failed: "<<operation<<'\n';return passed?0:1;
}
