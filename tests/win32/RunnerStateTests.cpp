#include "win32/EmulatorRunner.h"
#include "win32/EmulationSession.h"
#include "FakeAudioConsumer.h"
#include "../system/Stage5TestRom.h"
#include <winioctl.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace smsgg::win32 {
struct RunnerTestAccess {static void requestStop(EmulatorRunner& runner) {runner.requestStop();}};
}
namespace {
using namespace smsgg;using namespace smsgg::win32;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
template<class Predicate> std::unique_ptr<RunnerSnapshot> waitFor(EmulatorRunner& runner,Predicate predicate) {
    const auto end=GetTickCount64()+10000;
    for(;;) {
        const auto now=GetTickCount64();require(now<end,"runner timeout");
        require(WaitForSingleObject(runner.notificationEvent(),static_cast<DWORD>(end-now))==WAIT_OBJECT_0,"runner event timeout");
        auto snapshot=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());if(predicate(*snapshot)) return snapshot;
    }
}
std::unique_ptr<RunnerSnapshot> done(EmulatorRunner& runner,uint64_t serial) {
    return waitFor(runner,[&](const auto& s){return !s.busy && s.operationSerial>serial;});
}
std::vector<uint8_t> bytes(const std::filesystem::path& path) {std::ifstream f(path,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& data) {
    std::ofstream f(path,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),data.size());require(bool(f),"fixture write");
}
LoadRequest fixture(const std::filesystem::path& dir,bool ram=false) {
    auto data=test::stage5InputRom();data.resize(32768);
    if(ram) {
        // Enable cartridge RAM, set its first byte, then enter the existing test ROM.
        data[1]=0;data[2]=8;
        const uint8_t code[]{0x3e,8,0x32,0xfc,0xff,0x3e,0x5a,0x32,0,0xa0,0xc3,0,1};
        std::copy(std::begin(code),std::end(code),data.begin()+0x800);
    }
    const auto path=dir/(ram?L"ram.sms":L"input.sms");write(path,data);
    return {path,{ram?MapperKind::Sega:MapperKind::RomOnly,ram?8192u:0u}};
}
std::unique_ptr<RunnerSnapshot> loadPaused(EmulatorRunner& runner,const LoadRequest& request) {
    runner.setPaused(true);require(runner.load(request),"load rejected");
    auto s=done(runner,0);require(s->error.empty() && s->state==RunnerState::Paused,"load failed");return s;
}
std::unique_ptr<RunnerSnapshot> runThenPause(EmulatorRunner& runner,uint64_t frame) {
    runner.setPaused(false);waitFor(runner,[&](const auto& s){return s.frames>=frame;});
    runner.setPaused(true);return waitFor(runner,[](const auto& s){return s.state==RunnerState::Paused;});
}
void state(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);
    loadPaused(runner,fixture(dir));auto saved=runThenPause(runner,3);
    runner.setMuted(true);saved=waitFor(runner,[](const auto& s){return s.muted;});
    require(runner.saveState(dir/L"game.state"),"save rejected");auto s=done(runner,saved->operationSerial);
    require(s->error.empty() && s->tstates==saved->tstates && s->frames==saved->frames && s->muted,"save changed state");
    require(s->discardedQueued==saved->discardedQueued && s->discardedPending==saved->discardedPending && s->generated==saved->generated,"save flushed PCM");
    auto future=runThenPause(runner,s->frames+3);
    require(runner.loadState(dir/L"game.state"),"state load rejected");s=done(runner,future->operationSerial);
    require(s->error.empty() && s->tstates==saved->tstates && s->frames==saved->frames && s->state==RunnerState::Paused && s->muted,"state not restored");
    require(s->frameChanged && !s->romChanged && s->romPath==saved->romPath && s->queued==0 && s->pending==0,"state publication/PCM mismatch");
    require(std::equal(s->pixels.begin(),s->pixels.end(),saved->pixels.begin(),[](auto a,auto b){return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a;}),"wrong restored pixels");
    runThenPause(runner,s->frames+2);
}
void failure(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);
    const auto request=fixture(dir);
    loadPaused(runner,request);auto old=runThenPause(runner,2);
    require(runner.saveState(dir/L"bad.state"),"save rejected");old=done(runner,old->operationSerial);
    auto data=bytes(dir/L"bad.state");data.back()^=1;write(dir/L"bad.state",data);
    const auto image=std::get<RomImage>(RomLoader::load(request.path,request.options));
    EmulationSession candidate(image,request.path,VideoStandard::Ntsc,48000);
    auto state=std::make_unique<EmulatorSystemState>();
    require(std::holds_alternative<uint32_t>(candidate.system().step()) && candidate.system().tstates()==10,
        "CRAM fixture instruction");
    candidate.system().captureState(*state);
    state->vdp.cramChangeCount=2;
    state->vdp.cramChanges[0].dot=state->vdp.cramChanges[1].dot=cramWriteDot(state->tstates);
    write(dir/L"bad-cram.state",encodeState(candidate.profile(),*state));
    for(const auto& path:{dir/L"missing.state",dir/L"bad.state",dir/L"bad-cram.state"}) {
        require(runner.loadState(path),"load rejected");auto s=done(runner,old->operationSerial);
        require(!s->error.empty() && s->tstates==old->tstates && s->frames==old->frames && s->romPath==old->romPath,"failed load changed session");
        require(s->discardedQueued==old->discardedQueued && s->discardedPending==old->discardedPending && s->state==RunnerState::Paused,"failed load flushed");old=std::move(s);
    }
    const auto blocked=dir/L"blocked.state";
    std::filesystem::create_directory(blocked);
    require(runner.saveState(blocked),"save rejected");auto s=done(runner,old->operationSerial);
    require(!s->error.empty() && s->tstates==old->tstates && std::filesystem::is_empty(blocked),"failed save damaged session");
    std::filesystem::remove(blocked);
}
void ack(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio(false);EmulatorRunner runner(audio.stream);auto s=loadPaused(runner,fixture(dir));
    require(runner.saveState(dir/L"zero.state"),"save rejected");s=done(runner,s->operationSerial);
    runner.setPaused(false);s=waitFor(runner,[&](const auto& p){return p.queued==audio.stream.targetFrames();});
    const auto oldTime=s->tstates;const auto queued=s->queued;audio.holdStop(true);
    require(runner.loadState(dir/L"zero.state"),"load rejected");
    const auto end=GetTickCount64()+5000;
    while(audio.stream.control().command!=AudioCommand::Stopped && GetTickCount64()<end) Sleep(1);
    const auto control=audio.stream.control();
    const bool held=control.command==AudioCommand::Stopped && audio.stream.status().acknowledged<control.sequence;
    auto busy=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
    const bool rejected=!runner.saveState(dir/L"must-not-exist.state") && !runner.load(fixture(dir));
    const bool intact=busy->busy && busy->tstates==oldTime && audio.stream.pcm.queued()==queued;
    // Always release the test-owned acknowledgement before an assertion can unwind the runner.
    runner.setPaused(true);audio.holdStop(false);s=done(runner,s->operationSerial);
    require(held && rejected && intact,"candidate published or request overwritten before ack");
    require(s->tstates==0 && s->pending==0 && s->queued==0 && s->discardedQueued>=queued,"ack did not gate swap");
}
void sram(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);const auto request=fixture(dir,true);
    const auto image=std::get<RomImage>(RomLoader::load(request.path,request.options));SramStore store(request.path,image);
    std::filesystem::remove(store.path());auto s=loadPaused(runner,request);s=runThenPause(runner,2);
    require(s->sramEnabled && s->sramPath==store.path(),"SRAM metadata");
    std::filesystem::create_directory(store.path());require(runner.load(request),"reload rejected");auto failed=done(runner,s->operationSerial);
    require(failed->sramSaveFailed && !failed->error.empty() && failed->tstates==s->tstates && failed->romPath==s->romPath,"failed old SRAM save exchanged ROM");
    std::filesystem::remove(store.path());require(runner.load(request),"retry rejected");s=done(runner,failed->operationSerial);
    require(s->error.empty() && s->tstates==0 && bytes(store.path()).size()==8192 && bytes(store.path())[0]==0x5a,"same ROM SRAM order");
    require(runner.saveState(dir/L"reloaded.state"),"save rejected");auto saved=done(runner,s->operationSerial);
    EmulationSession expected(image,request.path,VideoStandard::Ntsc,48000);
    auto decoded=decodeState(bytes(dir/L"reloaded.state"),expected.profile());
    require(saved->error.empty() && std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(decoded) && std::get<std::unique_ptr<EmulatorSystemState>>(decoded)->cartridge.ram[0]==0x5a,"candidate did not read saved SRAM");
    auto& state=*std::get<std::unique_ptr<EmulatorSystemState>>(decoded);state.cartridge.ram[0]=0xa7;
    write(dir/L"manual.state",encodeState(expected.profile(),state));require(runner.loadState(dir/L"manual.state"),"manual SRAM fixture rejected");s=done(runner,saved->operationSerial);
    require(runner.saveSram(),"manual SRAM save rejected");s=done(runner,s->operationSerial);
    require(s->error.empty() && bytes(store.path())[0]==0xa7,"manual SRAM save did not write restored RAM");
    const auto lock=CreateFileW(store.path().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"lock SRAM destination");
    const auto accepted=runner.saveSram();saved=done(runner,s->operationSerial);
    CloseHandle(lock);
    require(accepted && saved->error.empty() && bytes(store.path())[0]==0xa7,"unchanged SRAM performed I/O");
}
void close(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);const auto request=fixture(dir,true);
    const auto image=std::get<RomImage>(RomLoader::load(request.path,request.options));SramStore store(request.path,image);
    std::filesystem::remove(store.path());auto s=loadPaused(runner,request);s=runThenPause(runner,2);
    std::filesystem::create_directory(store.path());require(runner.prepareClose(),"close rejected");auto failed=done(runner,s->operationSerial);
    require(failed->sramSaveFailed && !failed->closeReady && failed->tstates==s->tstates,"failed close lost session");
    require(runner.prepareClose(true),"discard rejected");s=done(runner,failed->operationSerial);
    require(s->closeReady && s->error.empty() && !runner.load(request) && !runner.saveState(dir/L"late.state"),"close readiness");
    runner.setPaused(false);require(WaitForSingleObject(runner.notificationEvent(),50)==WAIT_TIMEOUT,"prepared close advanced core");
    runner.stop();const auto stopped=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());require(stopped->tstates==s->tstates && stopped->state==RunnerState::Stopped,"close step after save");
    require(std::filesystem::is_empty(store.path()),"discard unexpectedly saved");std::filesystem::remove(store.path());
}
void fm(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);auto request=fixture(dir);request.fmEnabled=true;
    request.fmPlugin=L"acesms-fm-ymfm.dll";request.fmMethod=FmInterpolation::Linear;auto s=loadPaused(runner,request);
    require(s->fmProfile && s->fmProfile->method==FmInterpolation::Linear && s->fmBackendName=="ymfm OPLL" && s->fmProfile->clock==3579545 && s->fmProfile->divider==72,"current FM metadata");
    require(runner.saveState(dir/L"ymfm.state"),"FM save rejected");s=done(runner,s->operationSerial);require(s->error.empty(),"FM save failed");
    request.fmPlugin=L"missing.dll";require(runner.load(request),"missing plugin rejected at queue");auto bad=done(runner,s->operationSerial);
    require(!bad->error.empty() && bad->fmBackendName==s->fmBackendName && bad->fmProfile->method==FmInterpolation::Linear,"plugin fallback occurred");
    request.fmPlugin.clear();request.fmMethod=FmInterpolation::Sinc;require(runner.load(request),"builtin load rejected");s=done(runner,bad->operationSerial);
    require(s->error.empty() && s->fmProfile->method==FmInterpolation::Sinc,"builtin load failed");
    require(runner.loadState(dir/L"ymfm.state"),"mismatch load rejected");bad=done(runner,s->operationSerial);
    require(!bad->error.empty() && bad->fmBackendName==s->fmBackendName && bad->tstates==s->tstates,"state chose backend");
}
void running(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio(false);EmulatorRunner runner(audio.stream);auto s=loadPaused(runner,fixture(dir));
    runner.setMuted(true);runner.setPaused(false);s=waitFor(runner,[&](const auto& p){return p.queued==audio.stream.targetFrames() && p.muted;});
    require(runner.saveState(dir/L"running.state"),"running save rejected");auto saved=done(runner,s->operationSerial);
    require(saved->state==RunnerState::Running && saved->tstates==s->tstates && saved->queued==s->queued && saved->pending==s->pending,"running save altered delivery");
    require(saved->generated==s->generated && saved->discardedQueued==s->discardedQueued && saved->discardedPending==s->discardedPending,"running save flushed");
    require(runner.loadState(dir/L"missing.state"),"running load rejected");auto failed=done(runner,saved->operationSerial);
    require(!failed->error.empty() && failed->state==RunnerState::Running && failed->tstates==saved->tstates && failed->queued==saved->queued && failed->pending==saved->pending,"failed running load flushed");
    require(runner.loadState(dir/L"running.state"),"running restore rejected");auto restored=done(runner,failed->operationSerial);
    require(restored->error.empty() && restored->state==RunnerState::Running && restored->muted && restored->discardedQueued>=saved->queued,"running restore failed");
    audio.grant(480);s=waitFor(runner,[&](const auto& p){return p.tstates>saved->tstates && p.queued==audio.stream.targetFrames();});
}
void closeSave(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);const auto request=fixture(dir,true);
    auto image=std::get<RomImage>(RomLoader::load(request.path,request.options));SramStore store(request.path,image);std::filesystem::remove(store.path());
    auto s=loadPaused(runner,request);s=runThenPause(runner,2);runner.setPaused(false);
    require(runner.prepareClose(),"close save rejected");s=done(runner,s->operationSerial);
    require(s->closeReady && s->error.empty() && bytes(store.path()).size()==8192 && bytes(store.path())[0]==0x5a,"normal close did not save");
    const auto time=s->tstates;runner.setInput({.button1=true});runner.setPaused(false);audio.grant(480);
    require(WaitForSingleObject(runner.notificationEvent(),50)==WAIT_TIMEOUT,"prepared close advanced");
    runner.stop();s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());require(s->tstates==time,"prepared close time changed");
}
void candidate(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);auto oldRequest=fixture(dir,true);
    auto s=loadPaused(runner,oldRequest);s=runThenPause(runner,2);
    auto nextRequest=oldRequest;nextRequest.path=dir/L"next.sms";write(nextRequest.path,bytes(oldRequest.path));
    auto image=std::get<RomImage>(RomLoader::load(nextRequest.path,nextRequest.options));SramStore store(nextRequest.path,image);
    write(store.path(),{1});require(runner.load(nextRequest),"new ROM rejected");auto failed=done(runner,s->operationSerial);
    require(!failed->error.empty() && !failed->sramSaveFailed && failed->romPath==s->romPath && failed->tstates==s->tstates && failed->discardedQueued==s->discardedQueued,"bad candidate changed session");
    std::filesystem::remove(store.path());
    require(runner.saveState(dir/L"dirty.state"),"dirty fixture save rejected");s=done(runner,failed->operationSerial);
    EmulationSession profile(image,oldRequest.path,VideoStandard::Ntsc,48000);
    auto decoded=decodeState(bytes(dir/L"dirty.state"),profile.profile());
    require(std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(decoded),"dirty fixture decode");
    auto& state=*std::get<std::unique_ptr<EmulatorSystemState>>(decoded);state.cartridge.ram[0]=0xa7;
    write(dir/L"dirty.state",encodeState(profile.profile(),state));require(runner.loadState(dir/L"dirty.state"),"dirty restore rejected");s=done(runner,s->operationSerial);
    // Explicitly discard unsaved old RAM when switching to a different ROM.
    SramStore oldStore(oldRequest.path,image);
    nextRequest.discardSram=true;require(runner.load(nextRequest),"discard load rejected");auto next=done(runner,s->operationSerial);
    require(next->error.empty() && next->romPath==nextRequest.path && next->tstates==0,"explicit discard failed");
    require(bytes(oldStore.path())[0]==0x5a,"discard wrote old SRAM");
}
void fault(const std::filesystem::path& dir) {
    const auto path=dir/L"prefix.sms";write(path,test::stage5PrefixRom());
    LoadRequest request{path,{MapperKind::Sega}};
    auto image=std::get<RomImage>(RomLoader::load(path,request.options));
    EmulationSession prepared(image,path,VideoStandard::Ntsc,48000);
    auto state=std::make_unique<EmulatorSystemState>();prepared.system().captureState(*state);
    state->cpu.registers.pc=0xc001;state->cpu.halted=true;state->memory.ram[0]=0x76;
    require(prepared.system().restoreState(*state)==ACESMS_FM_OK && !prepared.saveState(dir/L"recovery.state"),"recovery fixture");
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);require(runner.load(request),"prefix load rejected");
    auto s=waitFor(runner,[](const auto& p){return p.state==RunnerState::Faulted;});
    require(!s->fatal && s->operationSucceeded && !runner.saveState(dir/L"fault.state"),"ROM load result lost at CPU fault");
    require(runner.loadState(dir/L"missing.state"),"fault load rejected");auto failed=done(runner,s->operationSerial);
    require(!failed->operationSucceeded && failed->state==RunnerState::Faulted && failed->tstates==s->tstates && !failed->error.empty(),"bad load cleared fault");
    runner.setPaused(true);require(runner.loadState(dir/L"recovery.state"),"recovery rejected");s=done(runner,failed->operationSerial);
    require(s->state==RunnerState::Paused && s->tstates==0 && s->error.empty(),"valid load did not clear CPU fault");
    runThenPause(runner,2);
}
void stopIo(const std::filesystem::path& dir) {
    test::FakeAudioConsumer audio;EmulatorRunner runner(audio.stream);auto s=loadPaused(runner,fixture(dir));s=runThenPause(runner,2);
    const auto path=dir/L"blocked.state";require(runner.saveState(path),"save rejected");s=done(runner,s->operationSerial);s=runThenPause(runner,s->frames+2);
    OVERLAPPED operation{};std::unique_ptr<void,decltype(&::CloseHandle)> event(CreateEventW(nullptr,TRUE,FALSE,nullptr),&::CloseHandle);
    require(bool(event),"oplock event");operation.hEvent=event.get();
    const auto raw=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    require(raw!=INVALID_HANDLE_VALUE,"oplock open");std::unique_ptr<void,decltype(&::CloseHandle)> held(raw,&::CloseHandle);DWORD count{};
    const auto granted=DeviceIoControl(held.get(),FSCTL_REQUEST_OPLOCK_LEVEL_1,nullptr,0,nullptr,0,&count,&operation);
    require(!granted && GetLastError()==ERROR_IO_PENDING,"oplock grant");
    require(runner.loadState(path),"blocked load rejected");
    require(WaitForSingleObject(event.get(),5000)==WAIT_OBJECT_0 && GetOverlappedResult(held.get(),&operation,&count,FALSE),"oplock break");
    RunnerTestAccess::requestStop(runner);held.reset();runner.stop();auto stopped=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
    require(stopped->state==RunnerState::Stopped && stopped->tstates==s->tstates && stopped->frames==s->frames && !stopped->busy,"state published after stop");
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) return 1;const std::filesystem::path dir=argv[2];std::filesystem::create_directories(dir);const std::string_view op=argv[1];
        if(op=="state") state(dir);else if(op=="failure") failure(dir);else if(op=="ack") ack(dir);else if(op=="sram") sram(dir);else if(op=="close") close(dir);else if(op=="fm") fm(dir);
        else if(op=="running") running(dir);else if(op=="close_save") closeSave(dir);else if(op=="candidate") candidate(dir);else if(op=="fault") fault(dir);else if(op=="stop_io") stopIo(dir);else return 1;
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
