#include "win32/EmulationSession.h"
#include "win32/FmPlugin.h"
#include "core/cartridge/RomLoader.h"
#include "../system/Stage5TestRom.h"

#include <fstream>
#include <algorithm>
#include <iostream>
#include <string_view>

namespace {
using namespace smsgg;using namespace smsgg::win32;
RomImage image(MapperKind mapper=MapperKind::Sega) {
    auto bytes=test::stage5InputRom();bytes.resize(32768);
    bytes[1]=0;bytes[2]=8;size_t at=0x800;
    auto out=[&](uint8_t port,uint8_t value){for(auto byte:{uint8_t(0x3e),value,uint8_t(0xd3),port}) bytes[at++]=byte;};
    out(0x7f,0x83);out(0x7f,1);out(0x7f,0x90);out(0xf2,3);out(0xf0,0x30);out(0xf1,0x10);out(0xf0,0x10);out(0xf1,0x80);out(0xf0,0x20);out(0xf1,0x19);
    bytes[at++]=0xc3;bytes[at++]=0;bytes[at]=1;
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{mapper,mapper==MapperKind::RomOnly?0u:8192u}));
}
std::vector<uint8_t> disk(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
std::vector<AudioFrame> advance(EmulatorSystem& system,unsigned count) {
    std::vector<AudioFrame> result;
    for(unsigned n=0;n<count;++n) {system.step({.button1=n%7<3},n==31);const auto frames=system.audioFrames();result.insert(result.end(),frames.begin(),frames.end());}
    return result;
}
bool state(const std::filesystem::path& dir,const FmBackendPtr& backend) {
    EmulationSession session(image(),dir/L"state.sms",VideoStandard::Ntsc,48000,Machine::Sms,true,FmInterpolation::Sinc,backend);
    if(session.loadSram()) return false;
    auto& system=session.system();advance(system,20000);const auto time=system.tstates();const auto delivery=system.audioFrames().size();
    if(session.saveState(dir/L"game.state") || system.tstates()!=time || system.audioFrames().size()!=delivery) return false;
    const auto expected=advance(system,20000);const auto future=system.tstates();
    auto prepared=session.prepareState(dir/L"game.state");if(!std::holds_alternative<std::unique_ptr<EmulatorSystem>>(prepared) || system.tstates()!=future) return false;
    session.commitState(std::get<std::unique_ptr<EmulatorSystem>>(std::move(prepared)));
    if(session.system().tstates()!=time || !session.system().audioFrames().empty()) return false;
    const auto actual=advance(session.system(),20000);
    if(actual.size()!=expected.size()) return false;
    if(std::none_of(expected.begin(),expected.end(),[](AudioFrame frame){return frame.left!=0 || frame.right!=0;})) return false;
    for(size_t n=0;n<actual.size();++n) if(actual[n].left!=expected[n].left || actual[n].right!=expected[n].right) return false;
    return session.system().tstates()==future;
}
bool failure(const std::filesystem::path& dir,const FmBackendPtr& backend) {
    EmulationSession session(image(),dir/L"failed.sms",VideoStandard::Ntsc,48000,Machine::Sms,true,FmInterpolation::Linear,backend);
    advance(session.system(),1000);const auto time=session.system().tstates();
    auto missing=session.prepareState(dir/L"missing.state");if(!std::holds_alternative<SessionError>(missing) || session.system().tstates()!=time) return false;
    auto saved=std::make_unique<EmulatorSystemState>();session.system().captureState(*saved);saved->audio.fm->backend[0]^=1;
    if(writeFileAtomically(dir/L"bad.state",encodeState(session.profile(),*saved))) return false;
    auto invalid=session.prepareState(dir/L"bad.state");
    if(!std::holds_alternative<SessionError>(invalid) || !std::holds_alternative<acesms_fm_result>(std::get<SessionError>(invalid)) || session.system().tstates()!=time) return false;
    auto bytes=encodeState(session.profile(),*saved);bytes.back()^=1;if(writeFileAtomically(dir/L"bad.state",bytes)) return false;
    invalid=session.prepareState(dir/L"bad.state");
    return std::holds_alternative<SessionError>(invalid) && std::holds_alternative<StateCodecError>(std::get<SessionError>(invalid)) && session.system().tstates()==time;
}
bool sram(const std::filesystem::path& dir) {
    const auto rom=image();SramStore store(dir/L"ram.sms",rom);std::filesystem::remove(store.path());
    EmulationSession session(rom,dir/L"ram.sms",VideoStandard::Ntsc,48000);if(session.loadSram()) return false;
    auto snapshot=std::make_unique<EmulatorSystemState>();session.system().captureState(*snapshot);snapshot->cartridge.ram[0]=0x5a;
    if(session.system().restoreState(*snapshot)!=ACESMS_FM_OK || session.saveSram() || session.saveState(dir/L"old.state")) return false;
    snapshot->cartridge.ram[0]=0xa7;session.system().restoreState(*snapshot);if(session.saveSram() || disk(store.path())[0]!=0xa7) return false;
    auto old=session.prepareState(dir/L"old.state");if(!std::holds_alternative<std::unique_ptr<EmulatorSystem>>(old)) return false;
    session.commitState(std::get<std::unique_ptr<EmulatorSystem>>(std::move(old)));if(disk(store.path())[0]!=0xa7) return false;
    if(session.saveSram() || disk(store.path())[0]!=0x5a) return false;
    EmulationSession again(rom,dir/L"ram.sms",VideoStandard::Ntsc,48000);if(again.loadSram()) return false;
    again.system().captureState(*snapshot);return snapshot->cartridge.ram[0]==0x5a;
}
bool disabled(const std::filesystem::path& dir) {
    EmulationSession session(image(MapperKind::Codemasters),dir/L"absent"/L"cm.sms",VideoStandard::Ntsc,48000);
    return !session.hasSram() && !session.loadSram() && !session.saveSram() && !std::filesystem::exists(dir/L"absent");
}
}
int main(int argc,char** argv) {
    if(argc<3) return 1;const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);FmBackendPtr backend;
    if(argc==4) {const auto dll=std::filesystem::absolute(argv[3]);auto loaded=loadFmPlugin(dll.parent_path(),dll.filename());if(!std::holds_alternative<FmBackendPtr>(loaded)) return 1;backend=std::get<FmBackendPtr>(std::move(loaded));}
    const std::string_view op=argv[1];const bool pass=op=="state"?state(dir,backend):op=="failure"?failure(dir,backend):op=="sram"?sram(dir):op=="disabled"&&disabled(dir);
    if(!pass) std::cerr<<"Session failed: "<<op<<'\n';return pass?0:1;
}
