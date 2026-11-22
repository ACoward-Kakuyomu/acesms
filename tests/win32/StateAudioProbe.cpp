#include "win32/AudioOutput.h"
#include "win32/EmulatorRunner.h"
#include "win32/EmulationSession.h"
#include "win32/FmPlugin.h"
#include "../system/Stage5TestRom.h"
#include "../system/Stage7TestRom.h"
#include "../system/Stage9TestRom.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace {
using namespace smsgg;using namespace smsgg::win32;
void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
struct Case {LoadRequest request;FmBackendPtr backend;};
Case configuration(const std::string& name,const std::filesystem::path& root) {
    Case c{{root/"ui.sms",{MapperKind::Sega,8192}}};
    if(name=="pal") c.request.standard=VideoStandard::Pal;
    else if(name=="gg") {c.request.path=root/"gg.gg";c.request.options={MapperKind::RomOnly};c.request.machine=Machine::GameGear;}
    else if(name=="cm") {c.request.path=root/"cm.sms";c.request.options={MapperKind::Codemasters,8192};}
    else {
        require(name.starts_with("builtin-") || name.starts_with("ymfm-"),"Unknown native profile");
        c.request.fmEnabled=true;const auto method=name.substr(name.find('-')+1);
        require(method=="sinc" || method=="zoh" || method=="linear","Unknown interpolation");
        c.request.fmMethod=method=="sinc"?FmInterpolation::Sinc:method=="zoh"?FmInterpolation::ZohLowPass:FmInterpolation::Linear;
        if(name.starts_with("ymfm-")) {
            c.request.fmPlugin=L"acesms-fm-ymfm.dll";auto loaded=loadFmPlugin(fmPluginDirectory(),c.request.fmPlugin);
            if(const auto* error=std::get_if<std::string>(&loaded)) throw std::runtime_error(*error);
            c.backend=std::get<FmBackendPtr>(std::move(loaded));
        }
    }
    return c;
}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());require(bool(out),"Write original fixture");
}
void fixtures(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);auto bytes=test::stage5InputRom();bytes.resize(32768);bytes[1]=0;bytes[2]=8;
    size_t at=0x800;for(uint8_t b:{0x3e,8,0x32,0xfc,0xff,0x3e,0x5a,0x32,0,0xa0}) bytes[at++]=b;
    const auto out=[&](uint8_t port,uint8_t value){for(auto b:{uint8_t(0x3e),value,uint8_t(0xd3),port}) bytes[at++]=b;};
    out(0x7f,0x83);out(0x7f,1);out(0x7f,0x90);out(0xf2,3);out(0xf0,0x30);out(0xf1,0x10);out(0xf0,0x10);out(0xf1,0x80);out(0xf0,0x20);out(0xf1,0x19);
    bytes[at++]=0xc3;bytes[at++]=0;bytes[at]=1;write(root/"ui.sms",bytes);write(root/"gg.gg",test::stage7GameGearRom());write(root/"cm.sms",test::stage9CodemastersRom(false,true));
    const auto rom=std::get<RomImage>(RomLoader::load(root/"ui.sms",{MapperKind::Sega,8192}));
    const auto path=SramStore(root/"ui.sms",rom).path();const auto text=path.u8string();
    std::ofstream manifest(root/"sram-path.txt",std::ios::binary);manifest.write(reinterpret_cast<const char*>(text.data()),text.size());require(bool(manifest),"Write SRAM path");
    std::cout<<"Original fixtures ready; ui ROM bytes="<<rom.bytes().size()<<" CRC32="<<std::hex<<rom.metadata().crc32<<std::dec<<'\n';
}
template<class Predicate> std::unique_ptr<RunnerSnapshot> wait(EmulatorRunner& runner,Predicate predicate) {
    const auto end=GetTickCount64()+10000;
    for(;;) {
        auto s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());require(!s->fatal && s->error.empty(),s->error.c_str());if(predicate(*s)) return s;
        const auto now=GetTickCount64();require(now<end,"Native worker timeout");require(WaitForSingleObject(runner.notificationEvent(),static_cast<DWORD>(end-now))==WAIT_OBJECT_0,"Native worker event");
    }
}
auto done(EmulatorRunner& runner,uint64_t serial) {return wait(runner,[&](const auto& s){return !s.busy && s.operationSerial>serial;});}
void inspect(const std::string& name,const std::filesystem::path& root,const std::filesystem::path& file,uint32_t rate,bool initial) {
    auto c=configuration(name,root);auto image=std::get<RomImage>(RomLoader::load(c.request.path,c.request.options));
    EmulationSession reference(std::move(image),c.request.path,c.request.standard,rate,*c.request.machine,c.request.fmEnabled,c.request.fmMethod,c.backend);
    auto prepared=reference.prepareState(file);require(std::holds_alternative<std::unique_ptr<EmulatorSystem>>(prepared),"Inspect state/profile rejected");
    auto state=std::make_unique<EmulatorSystemState>();std::get<std::unique_ptr<EmulatorSystem>>(prepared)->captureState(*state);
    if(initial) require(state->tstates==0,"Restart initial state stepped");
    if(c.request.options.mapper==MapperKind::Sega) require(state->cartridge.ram[0]==0x5a,"Persistent RAM missing after restart");
    if(initial && c.request.options.mapper==MapperKind::Codemasters) require(std::ranges::all_of(state->cartridge.ram,[](uint8_t b){return b==0;}),"Volatile CM RAM persisted");
    std::cout<<name<<": state validated T="<<state->tstates<<" rate="<<rate<<" RAM="<<state->cartridge.ram.size()<<'\n';
}
void native(const std::string& name,const std::filesystem::path& root) {
    auto c=configuration(name,root);AudioOutput audio;const auto rate=audio.stream().sampleRate();
    auto image=std::get<RomImage>(RomLoader::load(c.request.path,c.request.options));
    EmulationSession reference(std::move(image),c.request.path,c.request.standard,rate,*c.request.machine,true,c.request.fmMethod,c.backend);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream());runner->setPaused(true);require(runner->load(c.request),"Native ROM rejected");auto s=done(*runner,0);
    runner->setPaused(false);s=wait(*runner,[](const auto& v){return v.frames>=20;});
    const auto started=std::chrono::steady_clock::now();const auto firstT=s->tstates;
    s=wait(*runner,[&](const auto& v){return v.tstates>=firstT+2*cpuClockHz(c.request.standard);});
    runner->setPaused(true);s=wait(*runner,[](const auto& v){return v.state==RunnerState::Paused;});
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    const auto ratio=double(s->tstates-firstT)/cpuClockHz(c.request.standard)/elapsed;require(ratio>.95 && ratio<1.05,"Native speed smoke range");
    const auto savedT=s->tstates;const auto file=root/(name+"-native.state");require(runner->saveState(file),"Native save rejected");s=done(*runner,s->operationSerial);require(s->tstates==savedT,"Save stepped native core");
    runner->setPaused(false);s=wait(*runner,[&](const auto& v){return v.tstates>savedT+cpuClockHz(c.request.standard)/2;});
    runner->setPaused(true);s=wait(*runner,[](const auto& v){return v.state==RunnerState::Paused;});runner->setMuted(true);
    require(runner->loadState(file),"Native restore rejected");s=done(*runner,s->operationSerial);require(s->tstates==savedT && s->muted && !s->queued && !s->pending,"Native restore/ack mismatch");
    const auto generated=s->generated;runner->setPaused(false);s=wait(*runner,[&](const auto& v){return v.tstates>savedT+cpuClockHz(c.request.standard);});
    runner->setPaused(true);s=wait(*runner,[](const auto& v){return v.state==RunnerState::Paused;});const auto finalT=s->tstates;
    const auto after=root/(name+"-after.state");require(runner->saveState(after),"Native final save rejected");s=done(*runner,s->operationSerial);
    auto prepared=reference.prepareState(file);require(std::holds_alternative<std::unique_ptr<EmulatorSystem>>(prepared),"Headless native candidate");auto expected=std::get<std::unique_ptr<EmulatorSystem>>(std::move(prepared));
    uint64_t pcm=0;bool audible=false;
    while(expected->tstates()<finalT) {require(!std::holds_alternative<CpuStepError>(expected->step()),"Headless CPU fault");for(auto f:expected->audioFrames()) {++pcm;audible|=f.left!=0 || f.right!=0;}}
    auto state=std::make_unique<EmulatorSystemState>();expected->captureState(*state);const auto encoded=encodeState(reference.profile(),*state);auto actual=readFileExact(after,encoded.size());
    require(expected->tstates()==finalT && std::holds_alternative<std::vector<uint8_t>>(actual) && std::get<std::vector<uint8_t>>(actual)==encoded,"Native/headless full state differs");
    require(audible && s->generated-generated==pcm && !s->audio.silence && s->generated==s->enqueued+s->discardedPending && s->enqueued==s->audio.consumed+s->discardedQueued,"Native PCM/accounting mismatch");
    std::cout<<name<<": "<<audio.description()<<" speedRatio="<<ratio<<" T="<<savedT<<" -> "<<finalT<<" replayPCM="<<pcm<<" consumer="<<s->audio.consumed<<" silence="<<s->audio.silence<<" fullStateBytes="<<encoded.size()<<" MATCH"<<std::endl;
    require(runner->prepareClose(),"Native close rejected");s=done(*runner,s->operationSerial);require(s->closeReady,"Native close not ready");runner->stop();runner.reset();audio.stop();require(audio.stream().status().exited,"Audio not stopped");
}
}
int main(int argc,char** argv) {
    try {
        if(argc<3) return 1;const std::string op=argv[1];const std::filesystem::path root=std::filesystem::absolute(argv[2]);
        if(op=="fixture") fixtures(root);
        else if(op=="matrix") {fixtures(root);for(auto backend:{"builtin","ymfm"}) for(auto method:{"sinc","zoh","linear"}) native(std::string(backend)+'-'+method,root);}
        else if(op=="inspect" && argc==7) inspect(argv[3],root,argv[4],static_cast<uint32_t>(std::stoul(argv[5])),std::string_view(argv[6])=="initial");
        else return 1;return 0;
    } catch(const std::exception& e) {std::cerr<<"State/audio acceptance: "<<e.what()<<'\n';return 1;}
}
