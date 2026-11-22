#include "win32/EmulatorRunner.h"
#include "FakeAudioConsumer.h"
#include "../system/Stage6TestRom.h"

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>

using namespace smsgg;
using namespace smsgg::win32;
namespace {
template<class Predicate>
std::unique_ptr<RunnerSnapshot> wait(EmulatorRunner& runner,Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        auto s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
        if(predicate(*s)) return s;
        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(left<=0 || WaitForSingleObject(runner.notificationEvent(),static_cast<DWORD>(left))!=WAIT_OBJECT_0)
            throw std::runtime_error("Audio runner notification timeout");
    }
}
LoadRequest fixture(const std::filesystem::path& root,bool prefix=false) {
    std::filesystem::create_directories(root);
    const auto path=root/(prefix?"long.sms":"sound.sms");
    auto bytes=prefix?std::vector<uint8_t>(65536,0xdd):test::stage6AudioRom();
    if(prefix) bytes[0xbfff]=0;
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if(!file) throw std::runtime_error("ROM write failed");
    return {path,{prefix?MapperKind::Sega:MapperKind::RomOnly},prefix?VideoStandard::Pal:VideoStandard::Ntsc};
}
bool bounded(const std::filesystem::path& root,bool prefix) {
    test::FakeAudioConsumer audio(false,prefix?96000:44100);
    auto owned=std::make_unique<EmulatorRunner>(audio.stream); auto& runner=*owned;
    runner.load(fixture(root,prefix));
    auto s=wait(runner,[&](auto& v){return v.queued==audio.stream.targetFrames();});
    if(s->generated!=s->enqueued+s->pending || s->enqueued!=s->queued || s->audio.consumed || s->audio.silence) return false;
    if(prefix && (s->tstates!=196608 || s->pending==0)) return false;
    const auto time=s->tstates,generated=s->generated,enqueued=s->enqueued;
    // At H, no command or consumer activity can permit another instruction.
    if(WaitForSingleObject(runner.notificationEvent(),50)==WAIT_OBJECT_0) s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
    if(s->tstates!=time || s->generated!=generated) return false;
    audio.grant(13);
    s=wait(runner,[&](auto& v){return v.enqueued>=enqueued+13 && v.queued==audio.stream.targetFrames();});
    if(prefix && (s->tstates!=time || s->generated!=generated || s->enqueued!=enqueued+13)) return false;
    runner.stop();
    s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
    return s->state==RunnerState::Stopped && s->queued==0 && s->pending==0
        && s->generated==s->enqueued+s->discardedPending && s->enqueued==s->audio.consumed+s->discardedQueued;
}
bool pause(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false);
    auto owned=std::make_unique<EmulatorRunner>(audio.stream); auto& runner=*owned;
    runner.load(fixture(root));
    auto s=wait(runner,[&](auto& v){return v.queued==audio.stream.targetFrames() && v.audio.playing;});
    const auto before=std::make_unique<RunnerSnapshot>(*s);
    audio.holdStop(true);runner.setPaused(true);
    // Wake status snapshots until the stopped request is visible, but withhold its ack.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(audio.stream.control().command!=AudioCommand::Stopped) {
        if(std::chrono::steady_clock::now()>deadline) {audio.holdStop(false);throw std::runtime_error("Stop request timeout");}
        WaitForSingleObject(runner.notificationEvent(),10);
    }
    s=std::make_unique<RunnerSnapshot>(runner.takeSnapshot());
    if(s->state==RunnerState::Paused || s->discardedQueued || s->discardedPending) {audio.holdStop(false);return false;}
    audio.holdStop(false);
    s=wait(runner,[](auto& v){return v.state==RunnerState::Paused;});
    if(s->tstates!=before->tstates || s->queued || s->pending || s->audio.playing) return false;
    runner.load({root/"missing.sms",{MapperKind::RomOnly},VideoStandard::Ntsc});
    s=wait(runner,[](auto& v){return !v.error.empty();});
    if(s->state!=RunnerState::Paused || s->tstates!=before->tstates) return false;
    runner.setPaused(false);
    s=wait(runner,[&](auto& v){return v.tstates>before->tstates && v.queued==audio.stream.targetFrames();});
    runner.stop();return !s->fatal;
}

enum class DuringPause { Stop, Resume, Save };

bool pauseInterruption(const std::filesystem::path& root, DuringPause action) {
    test::FakeAudioConsumer audio(false);
    auto runner = std::make_unique<EmulatorRunner>(audio.stream);
    if (!runner->load(fixture(root))) return false;
    const auto before = wait(*runner, [&](const auto& snapshot) {
        return snapshot.audio.playing && snapshot.queued == audio.stream.targetFrames();
    });

    audio.holdStop(true);
    runner->setPaused(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (audio.stream.control().command != AudioCommand::Stopped) {
        if (std::chrono::steady_clock::now() >= deadline) {
            audio.holdStop(false);
            throw std::runtime_error("Pause stop request timeout");
        }
        WaitForSingleObject(runner->notificationEvent(), 10);
    }

    const auto saved = root / "paused.state";
    bool accepted = true;
    std::future<void> stopping;
    switch (action) {
    case DuringPause::Stop:
        stopping = std::async(std::launch::async, [&] { runner->stop(); });
        break;
    case DuringPause::Resume: runner->setPaused(false); break;
    case DuringPause::Save: accepted = runner->saveState(saved); break;
    }

    // Let the worker handle the wake while the consumer still withholds the ack.
    auto snapshot = std::make_unique<RunnerSnapshot>(runner->takeSnapshot());
    const bool stillWaiting = WaitForSingleObject(runner->notificationEvent(), 50) == WAIT_TIMEOUT &&
        snapshot->state == RunnerState::Running && snapshot->operationSerial == before->operationSerial;
    audio.holdStop(false);

    if (stopping.valid()) {
        const bool joined = stopping.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        if (!joined) audio.stream.wakeProducer(); // Release a stuck worker only after the failure has been observed.
        stopping.get();
        if (!joined) throw std::runtime_error("Stop during pause ack did not join");
        snapshot = std::make_unique<RunnerSnapshot>(runner->takeSnapshot());
        return stillWaiting && snapshot->state == RunnerState::Stopped &&
            !snapshot->fatal && !snapshot->queued && !snapshot->pending;
    }
    if (!accepted || !stillWaiting) return false;
    switch (action) {
    case DuringPause::Resume:
        snapshot = wait(*runner, [&](const auto& current) {
            return current.state == RunnerState::Running && current.tstates > before->tstates && current.audio.playing;
        });
        return !snapshot->fatal;
    case DuringPause::Save:
        snapshot = wait(*runner, [&](const auto& current) { return current.operationSerial > before->operationSerial; });
        return snapshot->operationSucceeded && snapshot->state == RunnerState::Paused &&
            snapshot->tstates == before->tstates && std::filesystem::file_size(saved) > 0;
    case DuringPause::Stop: break;
    }
    return false;
}

bool mute(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio;
    auto owned=std::make_unique<EmulatorRunner>(audio.stream); auto& runner=*owned;
    runner.load(fixture(root));
    auto s=wait(runner,[](auto& v){return v.frames>=3;});
    const auto time=s->tstates;
    runner.setMuted(true);
    s=wait(runner,[&](auto& v){return v.muted && v.tstates>time && v.audio.consumed>0;});
    if(!audio.stream.control().muted || s->discardedQueued || s->discardedPending) return false;
    runner.setMuted(false);runner.stop();return true;
}
bool failure(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false);
    auto owned=std::make_unique<EmulatorRunner>(audio.stream); auto& runner=*owned;
    runner.load(fixture(root));
    wait(runner,[&](auto& v){return v.queued==audio.stream.targetFrames();});
    audio.stream.fail("Injected endpoint invalidation");
    auto s=wait(runner,[](auto& v){return v.fatal;});
    runner.stop();
    return s->state==RunnerState::Faulted && s->error=="Injected endpoint invalidation";
}
bool stopAck(const std::filesystem::path& root,bool fail) {
    test::FakeAudioConsumer audio(false);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    runner->load(fixture(root));
    wait(*runner,[&](auto& v){return v.queued==audio.stream.targetFrames() && v.audio.playing;});
    audio.holdStop(true);
    auto stopped=std::async(std::launch::async,[&]{runner->stop();});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(audio.stream.control().command!=AudioCommand::Stopped && std::chrono::steady_clock::now()<deadline)
        WaitForSingleObject(runner->notificationEvent(),10);
    const bool waiting=audio.stream.control().command==AudioCommand::Stopped
        && stopped.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout;
    if(fail) audio.stream.fail("Injected failure during stop ack");
    audio.holdStop(false);
    if(stopped.wait_for(std::chrono::seconds(2))!=std::future_status::ready) throw std::runtime_error("Stop did not join");
    stopped.get();
    const auto s=std::make_unique<RunnerSnapshot>(runner->takeSnapshot());
    return waiting && s->state==RunnerState::Stopped
        && (fail ? s->queued>0 && !audio.stream.status().error.empty() : s->queued==0 && s->pending==0);
}
bool loadFlush(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    const auto load=fixture(root);runner->load(load);
    auto s=wait(*runner,[&](auto& v){return v.queued==audio.stream.targetFrames();});
    const auto time=s->tstates,generated=s->generated,queued=s->queued;
    runner->load({root/"absent.sms",{MapperKind::RomOnly},VideoStandard::Ntsc});
    s=wait(*runner,[](auto& v){return !v.error.empty();});
    if(s->state!=RunnerState::Running || s->tstates!=time || s->romPath!=load.path
        || s->discardedQueued || s->queued!=queued || s->generated!=generated) return false;
    runner->load(load);
    s=wait(*runner,[](auto& v){return v.romChanged && v.state==RunnerState::Running;});
    if(s->discardedQueued!=queued) return false;
    s=wait(*runner,[&](auto& v){return v.generated>generated && v.queued==audio.stream.targetFrames();});
    runner->setPaused(true);s=wait(*runner,[](auto& v){return v.state==RunnerState::Paused;});
    runner->load(load);s=wait(*runner,[](auto& v){return v.romChanged && v.state==RunnerState::Paused;});
    runner->stop();return s->tstates==0 && s->queued==0 && s->pending==0;
}
bool machine(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    LoadRequest invalid{root/"absent.gg",{MapperKind::RomOnly},VideoStandard::Pal,Machine::GameGear};
    runner->load(invalid);
    auto s=wait(*runner,[](auto& v){return !v.error.empty();});
    if(s->state!=RunnerState::Idle || s->error.find("NTSC")==std::string::npos || s->generated || s->discardedQueued) return false;
    auto load=fixture(root);runner->load(load);
    s=wait(*runner,[&](auto& v){return v.queued==audio.stream.targetFrames() && v.audio.playing;});
    const auto time=s->tstates,generated=s->generated,enqueued=s->enqueued;
    if(s->machine!=Machine::Sms || s->displayArea.width!=256 || s->displayArea.x) return false;
    runner->load(invalid);s=wait(*runner,[](auto& v){return !v.error.empty();});
    if(s->error.find("NTSC")==std::string::npos || s->tstates!=time || s->generated!=generated || s->enqueued!=enqueued
        || s->discardedQueued || s->discardedPending || s->machine!=Machine::Sms || s->displayArea.width!=256) return false;
    runner->setPaused(true);wait(*runner,[](auto& v){return v.state==RunnerState::Paused;});
    // This is an ownership/metadata test; the original GG ROM visual gate is separate.
    load.machine=Machine::GameGear;runner->load(load);
    s=wait(*runner,[](auto& v){return v.romChanged && v.state==RunnerState::Paused;});
    if(s->tstates || s->machine!=Machine::GameGear || s->displayArea.x!=48 || s->displayArea.y!=24
        || s->displayArea.width!=160 || s->displayArea.height!=144) return false;
    runner->load({root/"missing.sms",{MapperKind::RomOnly},VideoStandard::Ntsc,Machine::Sms});
    s=wait(*runner,[](auto& v){return !v.error.empty();});
    if(s->machine!=Machine::GameGear || s->displayArea.width!=160 || s->tstates) return false;
    load.machine=Machine::Sms;load.standard=VideoStandard::Pal;runner->load(load);
    s=wait(*runner,[](auto& v){return v.romChanged && v.state==RunnerState::Paused;});
    if(s->machine!=Machine::Sms || s->displayArea.width!=256 || s->displayArea.x || s->tstates) return false;
    runner->setPaused(false);s=wait(*runner,[&](auto& v){return v.queued==audio.stream.targetFrames();});
    runner->stop();return s->tstates>0 && !s->fatal;
}
bool fmProfile(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    auto load=fixture(root);load.fmEnabled=true;runner->load(load);
    auto s=wait(*runner,[&](auto& v){return v.queued==audio.stream.targetFrames() && v.audio.playing;});
    if(!s->fmEnabled || s->machine!=Machine::Sms) return false;
    const auto time=s->tstates,generated=s->generated,queued=s->queued;
    for(const auto machine:{Machine::Sms,Machine::GameGear}) {
        LoadRequest invalid{root/"absent.sms",{MapperKind::RomOnly},
            machine==Machine::Sms?VideoStandard::Pal:VideoStandard::Ntsc,machine,true};
        runner->load(invalid);s=wait(*runner,[](auto& v){return !v.error.empty();});
        if(s->error.find("FM")==std::string::npos || !s->fmEnabled || s->tstates!=time || s->generated!=generated
            || s->queued!=queued || s->discardedQueued || s->discardedPending) return false;
    }
    runner->setPaused(true);s=wait(*runner,[](auto& v){return v.state==RunnerState::Paused;});
    runner->load({root/"absent.sms",{MapperKind::RomOnly},VideoStandard::Ntsc,Machine::Sms,false});
    s=wait(*runner,[](auto& v){return !v.error.empty();});
    if(!s->fmEnabled || s->romPath!=load.path || s->tstates!=time) return false;
    load.fmEnabled=false;runner->load(load);
    s=wait(*runner,[](auto& v){return v.romChanged;});
    if(s->fmEnabled || s->tstates || s->queued || s->pending) return false;
    load.machine=Machine::GameGear;runner->load(load);
    s=wait(*runner,[](auto& v){return v.romChanged;});
    runner->stop();return s->machine==Machine::GameGear && !s->fmEnabled && s->state==RunnerState::Paused;
}
bool fastForward(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false,96000);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    runner->setFastForward(true);
    wait(*runner,[](const auto& s){return s.fastForward && s.state==RunnerState::Idle;});
    // One instruction produces more PCM than the entire queue can hold.
    if(!runner->load(fixture(root,true))) return false;
    const auto target=audio.stream.targetFrames();
    auto s=wait(*runner,[&](const auto& v){return v.audio.playing && v.tstates>196608 && v.discardedPending>0;});
    if(s->queued!=target || s->enqueued!=target || s->pending || s->audio.consumed ||
       s->generated!=s->enqueued+s->discardedPending) return false;
    const auto time=s->tstates;
    audio.grant(13);
    s=wait(*runner,[&](const auto& v){return v.tstates>time && v.enqueued==target+13 && v.queued==target && v.audio.consumed==13;});
    const auto valid=!s->pending && s->generated==s->enqueued+s->discardedPending;
    runner->stop();
    s=std::make_unique<RunnerSnapshot>(runner->takeSnapshot());
    return valid && s->state==RunnerState::Stopped && !s->queued && !s->pending &&
        s->generated==s->enqueued+s->discardedPending && s->enqueued==s->audio.consumed+s->discardedQueued;
}
bool fastForwardSwitch(const std::filesystem::path& root) {
    test::FakeAudioConsumer audio(false);
    auto runner=std::make_unique<EmulatorRunner>(audio.stream);
    const auto target=audio.stream.targetFrames();
    if(!runner->load(fixture(root))) return false;
    auto s=wait(*runner,[&](const auto& v){return v.queued==target && v.audio.playing;});
    for(const bool mode:{true,false}) {
        const auto flushed=s->discardedQueued;
        audio.holdStop(true);
        runner->setFastForward(mode);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(audio.stream.control().command!=AudioCommand::Stopped && std::chrono::steady_clock::now()<deadline)
            WaitForSingleObject(runner->notificationEvent(),10);
        const auto beforeAck=std::make_unique<RunnerSnapshot>(runner->takeSnapshot());
        const bool properlyWaiting=audio.stream.control().command==AudioCommand::Stopped &&
            audio.stream.pcm.queued()==target && beforeAck->fastForward!=mode && beforeAck->discardedQueued==flushed;
        audio.holdStop(false);
        if(!properlyWaiting) return false;
        s=wait(*runner,[&](const auto& v){return v.fastForward==mode && v.queued==target && v.audio.playing;});
        if(s->discardedQueued!=flushed+target || s->audio.consumed || s->tstates<=beforeAck->tstates) return false;
    }
    const auto time=s->tstates;
    WaitForSingleObject(runner->notificationEvent(),50);
    s=std::make_unique<RunnerSnapshot>(runner->takeSnapshot());
    if(s->tstates!=time) return false;
    runner->setPaused(true);
    s=wait(*runner,[](const auto& v){return v.state==RunnerState::Paused;});
    runner->setFastForward(true);
    s=wait(*runner,[](const auto& v){return v.fastForward;});
    if(s->state!=RunnerState::Paused || s->tstates!=time || s->queued || s->audio.playing) return false;
    runner->setPaused(false);
    s=wait(*runner,[&](const auto& v){return v.fastForward && v.tstates>time && v.queued==target && v.audio.playing;});
    return !s->fatal;
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) return 1;
        const std::string name=argv[1];const std::filesystem::path root=argv[2];
        const bool pass=name=="bounded"?bounded(root,false):name=="pending"?bounded(root,true)
            :name=="pause"?pause(root):name=="mute"?mute(root):name=="failure"?failure(root)
            :name=="pause_stop"?pauseInterruption(root,DuringPause::Stop)
            :name=="pause_resume"?pauseInterruption(root,DuringPause::Resume)
            :name=="pause_save"?pauseInterruption(root,DuringPause::Save)
            :name=="stop_ack"?stopAck(root,false):name=="stop_failure"?stopAck(root,true)
            :name=="fast_forward"?fastForward(root):name=="fast_forward_switch"?fastForwardSwitch(root)
            :name=="machine"?machine(root):name=="fm"?fmProfile(root):name=="load_flush"&&loadFlush(root);
        if(!pass) std::cerr<<"Runner audio test failed: "<<name<<'\n';
        return pass?0:1;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;}
}
