#include "win32/AudioOutput.h"
#include "win32/EmulatorRunner.h"
#include "../system/Stage6TestRom.h"
#include "../system/Stage7TestRom.h"
#include "../system/Stage8TestRom.h"

#include <chrono>
#include <fstream>
#include <iostream>

using namespace smsgg;
using namespace smsgg::win32;
namespace {
template<class Predicate>
std::unique_ptr<RunnerSnapshot> wait(EmulatorRunner& runner,Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    for(;;) {
        auto s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
        if(s->fatal || !s->error.empty()) throw std::runtime_error(s->error);
        if(predicate(*s)) return s;
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(left<=0 || WaitForSingleObject(runner.notificationEvent(),static_cast<DWORD>(left))!=WAIT_OBJECT_0)
            throw std::runtime_error("Native runner timeout");
    }
}
void print(const RunnerSnapshot& s,double elapsed) {
    std::cout<<"seconds="<<elapsed<<" T="<<s.tstates<<" frames="<<s.frames
        <<" G="<<s.generated<<" E="<<s.enqueued<<" D="<<s.audio.consumed<<" U="<<s.audio.silence
        <<" Q="<<s.queued<<" pending="<<s.pending<<" lastPadding="<<s.audio.padding
        <<" flushQ="<<s.discardedQueued<<" flushPending="<<s.discardedPending<<std::endl;
}
}
int main(int argc,char** argv) {
    try {
        if(argc<3) return 1;
        const auto seconds=std::stoi(argv[1]);
        const std::filesystem::path root=argv[2];
        const bool pal=argc>3 && std::string_view(argv[3])=="pal";
        const bool gg=argc>3 && std::string_view(argv[3])=="gg";
        const bool fm=argc>3 && std::string_view(argv[3])=="fm";
        std::filesystem::create_directories(root);
        const auto path=root/(gg?"native-gamegear.gg":fm?"native-fm.sms":"native-sound.sms");
        {const auto rom=gg?test::stage7GameGearRom():fm?test::stage8FmRom():test::stage6AudioRom();std::ofstream file(path,std::ios::binary);
            file.write(reinterpret_cast<const char*>(rom.data()),rom.size());if(!file) throw std::runtime_error("ROM write failed");}
        AudioOutput audio;
        std::cout<<audio.description()<<" standard="<<(pal?"PAL":"NTSC")<<" machine="<<(gg?"GG":"SMS")<<" FM="<<fm<<std::endl;
        auto runner=std::make_unique<EmulatorRunner>(audio.stream());
        runner->load({path,{MapperKind::RomOnly},pal?VideoStandard::Pal:VideoStandard::Ntsc,gg?Machine::GameGear:Machine::Sms,fm});
        auto s=wait(*runner,[](auto& v){return v.frames>=3;});
        if(s->machine!=(gg?Machine::GameGear:Machine::Sms) || s->fmEnabled!=fm || s->displayArea.width!=(gg?160u:256u)) return 1;
        const auto startTime=s->tstates;
        const auto start=std::chrono::steady_clock::now();
        print(*s,0);
        double next=30;
        while(std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)) {
            const auto previous=s->tstates;
            s=wait(*runner,[&](auto& v){return v.tstates>previous;});
            const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            if(elapsed>=next) {print(*s,elapsed);next+=30;}
        }
        runner->setPaused(true);
        s=wait(*runner,[](auto& v){return v.state==RunnerState::Paused;});
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const auto ratio=double(s->tstates-startTime)/cpuClockHz(pal?VideoStandard::Pal:VideoStandard::Ntsc)/elapsed;
        print(*s,elapsed);std::cout<<"speedRatio="<<ratio<<std::endl;
        if(s->audio.silence || ratio<.995 || ratio>1.005 || s->queued || s->pending || s->audio.playing
            || s->generated!=s->enqueued+s->discardedPending
            || s->enqueued!=s->audio.consumed+s->discardedQueued) return 1;
        const auto pausedTime=s->tstates;
        runner->setMuted(true);runner->setPaused(false);
        s=wait(*runner,[&](auto& v){return v.muted && v.tstates>pausedTime+cpuClockHz(pal?VideoStandard::Pal:VideoStandard::Ntsc);});
        runner->setPaused(true);s=wait(*runner,[](auto& v){return v.state==RunnerState::Paused;});
        if(s->audio.silence) return 1;
        runner->stop();runner.reset();audio.stop();
        return audio.stream().status().exited?0:1;
    } catch(const std::exception& e) {std::cerr<<"Native runner probe: "<<e.what()<<std::endl;return 2;}
}
