#include "win32/EmulatorRunner.h"
#include "FakeAudioConsumer.h"
#include "../cartridge/CodemastersTestSupport.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

using namespace smsgg;
using namespace smsgg::win32;
namespace {
template<class Predicate> auto wait(EmulatorRunner& runner,Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        auto s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
        if(predicate(*s)) return s;
        if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("Selection runner timeout: "+s->error);
        WaitForSingleObject(runner.notificationEvent(),10);
    }
}
std::filesystem::path fixture(const std::filesystem::path& root,uint8_t region) {
    auto bytes=test::codemastersHeaderFixture();
    bytes[0]=0x76; // HALT; include it in the independent fixed checksum.
    constexpr uint16_t sum=0x9503+0x76,negative=static_cast<uint16_t>(0u-sum);
    bytes[0x7fe6]=static_cast<uint8_t>(sum);bytes[0x7fe7]=static_cast<uint8_t>(sum>>8);
    bytes[0x7fe8]=static_cast<uint8_t>(negative);bytes[0x7fe9]=static_cast<uint8_t>(negative>>8);
    std::copy_n("TMR SEGA",8,bytes.begin()+0x7ff0);bytes[0x7fff]=static_cast<uint8_t>((region<<4)|0xc);
    const auto path=root/("model-"+std::to_string(region)+".sms");
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if(!out) throw std::runtime_error("Fixture write failed");return path;
}
bool run(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    test::FakeAudioConsumer audio(false);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    runner->setPaused(true);
    for(const uint8_t region:{3,4,5,6,7}) {
        runner->load({fixture(root,region),{MapperKind::Auto},VideoStandard::Ntsc,std::nullopt});
        auto s=wait(*runner,[](auto& v){return v.romChanged || !v.error.empty();});
        const auto machine=region<5?Machine::Sms:Machine::GameGear;
        if(!s->error.empty() || s->machine!=machine || s->mapper!=MapperKind::Codemasters || !s->romMetadata
            || s->romMetadata->mapperSelection!=MapperSelection::CodemastersHeader
            || !s->romMetadata->codemasters || s->romMetadata->codemasters->status!=ChecksumStatus::Match
            || s->state!=RunnerState::Paused || s->tstates || s->displayArea.width!=(region<5?256u:160u)) return false;
    }
    const auto path=fixture(root,6);
    runner->load({path,{MapperKind::Sega},VideoStandard::Ntsc,Machine::Sms});
    auto s=wait(*runner,[](auto& v){return v.romChanged;});
    if(s->mapper!=MapperKind::Sega || s->machine!=Machine::Sms || s->romMetadata->mapperSelection!=MapperSelection::Explicit) return false;
    runner->setPaused(false);s=wait(*runner,[&](auto& v){return v.queued==audio.stream.targetFrames();});
    const auto time=s->tstates,queued=s->queued;
    const auto crc=s->romMetadata->crc32;
    const LoadRequest invalid[]{
        {fixture(root,0),{MapperKind::Sega},VideoStandard::Ntsc,std::nullopt},
        {path,{MapperKind::Auto},VideoStandard::Pal,std::nullopt},
        {path,{MapperKind::Auto},VideoStandard::Ntsc,std::nullopt,true},
        {path,{MapperKind::Auto,16384},VideoStandard::Ntsc,Machine::Sms},
        {root/"missing.sms",{MapperKind::Sega},VideoStandard::Ntsc,Machine::Sms},
    };
    for(const auto& request:invalid) {
        runner->load(request);s=wait(*runner,[](auto& v){return !v.error.empty();});
        if(s->tstates!=time || s->queued!=queued || s->discardedQueued || s->discardedPending
            || s->mapper!=MapperKind::Sega || s->machine!=Machine::Sms || s->romMetadata->crc32!=crc) return false;
    }
    // RAM capacity is explicit even when machine/mapper are selected automatically.
    runner->load({path,{MapperKind::Codemasters,8192},VideoStandard::Ntsc,Machine::Sms});
    s=wait(*runner,[](auto& v){return v.romChanged || !v.error.empty();});
    if(!s->error.empty() || s->mapper!=MapperKind::Codemasters || s->romMetadata->crc32!=crc
        || s->state!=RunnerState::Running || s->discardedQueued!=queued) return false;
    runner->load({path,{MapperKind::Auto,8192},VideoStandard::Ntsc,std::nullopt});
    s=wait(*runner,[](auto& v){return v.romChanged || !v.error.empty();});
    if(!s->error.empty() || s->machine!=Machine::GameGear || s->mapper!=MapperKind::Codemasters
        || s->romMetadata->mapperSelection!=MapperSelection::CodemastersHeader) return false;
    runner->stop();return true;
}
}
int main(int argc,char** argv) {
    try {if(argc==2 && run(argv[1])) return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';}
    return 1;
}
