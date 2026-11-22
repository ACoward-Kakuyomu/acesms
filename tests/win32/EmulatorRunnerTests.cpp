#include "win32/EmulatorRunner.h"
#include "win32/EmulationSession.h"
#include "FakeAudioConsumer.h"
#include "../system/Stage5TestRom.h"

#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

using smsgg::win32::EmulatorRunner;
using smsgg::win32::RunnerSnapshot;
using smsgg::win32::RunnerState;
namespace smsgg::win32 {
struct RunnerTestAccess {
    static void requestStop(EmulatorRunner& runner) { runner.requestStop(); }
};
}
namespace {
template<class Predicate>
void waitFor(EmulatorRunner& runner, RunnerSnapshot& snapshot, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || WaitForSingleObject(runner.notificationEvent(), static_cast<DWORD>(left)) != WAIT_OBJECT_0)
            throw std::runtime_error("worker notification timeout/failure");
        snapshot = runner.takeSnapshot();
        if (predicate(snapshot)) return;
    } while (true);
}

void writeRom(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!stream) throw std::runtime_error("fixture write failed");
}
std::filesystem::path fixture(const std::filesystem::path& root) {
    const auto path = root / L"input \u65e5\u672c\u8a9e space.sms";
    writeRom(path, smsgg::test::stage5InputRom());
    return path;
}
smsgg::win32::LoadRequest request(const std::filesystem::path& path) {
    return {path, {smsgg::MapperKind::RomOnly}, smsgg::VideoStandard::Ntsc};
}
bool checker(const RunnerSnapshot& snapshot, uint8_t red, uint8_t green) {
    for (unsigned y = 0; y < 192; ++y) for (unsigned x = 0; x < 256; ++x) {
        const auto p = snapshot.pixels[y * 256 + x];
        const bool color = (x + y) % 2 == 0;
        if (p.r != (color ? red : 0) || p.g != (color ? green : 0)
            || p.b != (color ? 0 : 255) || p.a != 255) return false;
    }
    return true;
}

bool lifecycle(const std::filesystem::path& root) {
    smsgg::test::FakeAudioConsumer audio;
    auto runner = std::make_unique<EmulatorRunner>(audio.stream);
    auto snapshot = std::make_unique<RunnerSnapshot>();
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Idle; });
    if (snapshot->tstates || snapshot->frames || WaitForSingleObject(runner->notificationEvent(), 0) != WAIT_TIMEOUT) return false;
    runner->setPaused(true);
    runner->load(request(fixture(root)));
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Paused; });
    if (snapshot->tstates || snapshot->frames || !snapshot->frameChanged || !snapshot->romChanged) return false;
    for (const auto p : snapshot->pixels) if (p.r || p.g || p.b || p.a != 255) return false;
    // No timing-dependent sleep: a paused runner must not publish another update.
    if (WaitForSingleObject(runner->notificationEvent(), 50) != WAIT_TIMEOUT) return false;
    const auto still = runner->takeSnapshot();
    if (still.tstates || still.frames) return false;
    runner->setPaused(false);
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Running && s.frames >= 3; });
    if (!checker(*snapshot, 255, 0) || snapshot->frames != snapshot->tstates / (262 * 228)) return false;
    runner->setPaused(true);
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Paused; });
    const auto pausedTime = snapshot->tstates;
    if (WaitForSingleObject(runner->notificationEvent(), 50) != WAIT_TIMEOUT
        || runner->takeSnapshot().tstates != pausedTime) return false;
    runner->stop();
    *snapshot = runner->takeSnapshot();
    runner->stop();
    return snapshot->state == RunnerState::Stopped && snapshot->tstates == pausedTime;
}

bool loading(const std::filesystem::path& root) {
    smsgg::test::FakeAudioConsumer audio;
    auto runner = std::make_unique<EmulatorRunner>(audio.stream);
    auto snapshot = std::make_unique<RunnerSnapshot>();
    runner->load(request(root / "missing.sms"));
    waitFor(*runner, *snapshot, [](const auto& s) { return !s.error.empty(); });
    if (snapshot->state != RunnerState::Idle || !runner->takeSnapshot().error.empty()) return false;
    const auto path = fixture(root);
    runner->load(request(path));
    waitFor(*runner, *snapshot, [](const auto& s) { return s.frames >= 3; });
    runner->setPaused(true);
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Paused; });
    const auto time = snapshot->tstates, frames = snapshot->frames;
    runner->load(request(root / "missing.sms"));
    waitFor(*runner, *snapshot, [](const auto& s) { return !s.error.empty(); });
    if (snapshot->state != RunnerState::Paused || snapshot->tstates != time || snapshot->frames != frames
        || snapshot->romPath != path || !checker(*snapshot, 255, 0)) return false;
    auto badOptions = request(path);
    badOptions.options.sramBytes = 8192;
    runner->load(badOptions);
    waitFor(*runner, *snapshot, [](const auto& s) { return !s.error.empty(); });
    if (snapshot->state != RunnerState::Paused || snapshot->tstates != time) return false;
    if(!runner->load(request(root / "first-missing.sms"))) return false;
    waitFor(*runner,*snapshot,[](const auto& s){return !s.busy && !s.error.empty();});
    auto lastRequest = request(path);
    lastRequest.standard = smsgg::VideoStandard::Pal;
    // Stage 10 rejects requests while busy; submit the next ROM after completion.
    // runner_state_ack separately holds the consumer ack and verifies rejection without overwrite.
    if(!runner->load(lastRequest)) return false;
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.state == RunnerState::Paused && s.romPath == path && s.tstates == 0; });
    if (snapshot->frames || !snapshot->frameChanged || !snapshot->romChanged) return false;
    runner->setPaused(false);
    waitFor(*runner, *snapshot, [](const auto& s) { return s.frames >= 3; });
    if (snapshot->frames != snapshot->tstates / (313 * 228)) return false;
    runner->load(request(root / "running-missing.sms"));
    waitFor(*runner, *snapshot, [](const auto& s) { return !s.error.empty(); });
    if (snapshot->state != RunnerState::Running || snapshot->romPath != path) return false;
    const auto before = snapshot->frames;
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.frames > before; });
    runner->stop();
    return runner->takeSnapshot().state == RunnerState::Stopped;
}

bool inputAndFrames(const std::filesystem::path& root) {
    smsgg::test::FakeAudioConsumer audio;
    auto runner = std::make_unique<EmulatorRunner>(audio.stream);
    auto snapshot = std::make_unique<RunnerSnapshot>();
    runner->load(request(fixture(root)));
    waitFor(*runner, *snapshot, [](const auto& s) { return s.frames >= 3; });
    auto before = snapshot->frames;
    runner->setInput({.button1 = true});
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.frames >= before + 3; });
    if (!checker(*snapshot, 0, 255)) return false;
    before = snapshot->frames;
    runner->setInput({}, true);
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.frames >= before + 3; });
    if (!checker(*snapshot, 255, 255)) return false;
    runner->setPaused(true);
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Paused; });
    // Console Pause while host-paused is discarded; focus loss clears held buttons.
    runner->setInput({.button1 = true}, true);
    runner->clearInput();
    before = snapshot->frames;
    runner->setPaused(false);
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.frames >= before + 3; });
    if (!checker(*snapshot, 255, 255)) return false;
    before = snapshot->frames;
    runner->setInput({}, true);
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.frames >= before + 3; });
    if (!checker(*snapshot, 255, 0)) return false;
    const auto counterPath = root / "frame_counter.sms";
    writeRom(counterPath, smsgg::test::stage5InputRom(true));
    runner->load(request(counterPath));
    waitFor(*runner, *snapshot, [&](const auto& s) { return s.romPath == counterPath && s.frames >= 3; });
    // The ROM changes palette in vblank: published frame N has color (N-1) mod 4.
    // This detects torn pixels and mismatched frame metadata, without any GPU consumer.
    for (unsigned count = 0; count < 30; ++count) {
        const auto previousTime = snapshot->tstates;
        before = snapshot->frames;
        waitFor(*runner, *snapshot, [&](const auto& s) { return s.frames > before; });
        if (!snapshot->frameChanged || snapshot->tstates <= previousTime
            || !checker(*snapshot, static_cast<uint8_t>((snapshot->frames - 1) % 4 * 85), 0)
            || snapshot->frames != snapshot->tstates / (262 * 228)) return false;
    }
    auto twoPads=smsgg::test::stage5InputRom();
    const uint8_t readPad[]{0xdb,0xdc,0x32,0x02,0xc0,0xe6,0x3f,0xfe,0x3f};
    const auto poll=std::search(twoPads.begin(),twoPads.end(),std::begin(readPad),std::end(readPad));
    if(poll==twoPads.end()) return false;
    poll[1]=0xdd;
    poll[6]=poll[8]=0x04; // Display green on the active-low 2P button 1 line.
    const auto player2Path=root/"player2.sms";
    writeRom(player2Path,twoPads);
    if(!runner->load(request(player2Path))) return false;
    waitFor(*runner,*snapshot,[&](const auto& s){return s.romPath==player2Path && s.frames>=3;});
    before=snapshot->frames;
    runner->setInput({.button1=true});
    waitFor(*runner,*snapshot,[&](const auto& s){return s.frames>=before+3;});
    if(!checker(*snapshot,255,0)) return false;
    before=snapshot->frames;
    runner->setInput({},false,{.button1=true});
    waitFor(*runner,*snapshot,[&](const auto& s){return s.frames>=before+3;});
    if(!checker(*snapshot,0,255)) return false;
    before=snapshot->frames;
    runner->clearInput();
    waitFor(*runner,*snapshot,[&](const auto& s){return s.frames>=before+3;});
    if(!checker(*snapshot,255,0)) return false;
    runner->stop();
    return true;
}

bool rapidFire(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    const uint8_t program[]{0x21,0,0xc1,0x06,32, // LD HL,C100; LD B,32
        0xdb,0xdc,0x77,0x23,0xdb,0xdd,0x77,0x23, // Record both ports to RAM.
        0xdb,0xbf,0xcb,0x7f,0x28,0xfa,0x10,0xf0, // Wait for a VBlank; repeat.
        0x76,0xc3,21,0};
    for(const auto machine:{Machine::Sms,Machine::Sg1000,Machine::GameGear}) {
        std::vector<uint8_t> bytes(8192);
        std::copy(std::begin(program),std::end(program),bytes.begin());
        const auto path=root/(std::to_wstring(static_cast<int>(machine))+L".sms");
        writeRom(path,bytes);
        auto load=request(path);
        load.machine=machine;
        load.options.machine=machine;
        const auto rom=std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),load.options));
        EmulationSession reference(rom,path,load.standard,48000,machine);
        test::FakeAudioConsumer audio;
        auto runner=std::make_unique<EmulatorRunner>(audio.stream);
        auto snapshot=std::make_unique<RunnerSnapshot>();
        const auto wait=[&](auto predicate){waitFor(*runner,*snapshot,predicate);};
        runner->setPaused(true);
        runner->setFastForward(machine==Machine::Sg1000);
        if(!runner->load(load)) return false;
        wait([](const auto& s){return s.operationSucceeded && s.state==RunnerState::Paused;});
        runner->setInput({.button2=true},false,{.button1=true},0b1011);
        runner->setPaused(false);
        wait([](const auto& s){return s.frames>=14;});
        runner->setPaused(true);
        wait([](const auto& s){return s.state==RunnerState::Paused;});
        const auto currentState=root/L"rapid.state";
        const auto save=[&]() {
            const auto serial=snapshot->operationSerial;
            if(!runner->saveState(currentState)) throw std::runtime_error("rapid save submit");
            wait([&](const auto& s){return !s.busy && s.operationSerial>serial;});
            if(!snapshot->operationSucceeded) throw std::runtime_error(snapshot->error);
            auto candidate=reference.prepareState(currentState);
            return std::get<std::unique_ptr<EmulatorSystem>>(std::move(candidate));
        };
        const auto first=save();
        for(unsigned n=1;n<12;++n) {
            const bool on=((n-1)/2)%2==0;
            if(first->readMemory(static_cast<uint16_t>(0xc100+2*n))!=(on?0xcf:0xdf) ||
                first->readMemory(static_cast<uint16_t>(0xc101+2*n))!=(hasSecondPad(machine)?(on?0xf3:0xfb):0xff))
                throw std::runtime_error("rapid sequence, ordinary button priority or player isolation");
        }
        runner->setInput({},false,{},0);
        const auto before=snapshot->frames;
        runner->setPaused(false);
        wait([&](const auto& s){return s.frames>=before+3;});
        runner->setPaused(true);
        wait([](const auto& s){return s.state==RunnerState::Paused;});
        auto released=std::make_unique<EmulatorSystemState>();
        save()->captureState(*released);
        if(digitalPadBits(released->controller.pad) || digitalPadBits(released->controller.player2)) return false;

        runner->setInput({},false,{},0b1001);
        const auto serial=snapshot->operationSerial;
        if(!runner->loadState(currentState)) return false;
        wait([&](const auto& s){return !s.busy && s.operationSerial>serial;});
        if(!snapshot->operationSucceeded) return false;
        const auto restored=snapshot->frames;
        runner->setPaused(false);
        wait([&](const auto& s){return s.frames>=restored+3;});
        runner->setPaused(true);
        wait([](const auto& s){return s.state==RunnerState::Paused;});
        save()->captureState(*released);
        if(digitalPadBits(released->controller.pad) || digitalPadBits(released->controller.player2)) return false;
        runner->stop();
    }
    return true;
}

bool fault(const std::filesystem::path& root) {
    const auto badPath = root / "prefix.sms";
    writeRom(badPath, smsgg::test::stage5PrefixRom());
    smsgg::test::FakeAudioConsumer audio;
    auto runner = std::make_unique<EmulatorRunner>(audio.stream);
    auto snapshot = std::make_unique<RunnerSnapshot>();
    auto load = request(badPath);
    load.options.mapper = smsgg::MapperKind::Sega;
    runner->load(load);
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Faulted; });
    if (snapshot->error.empty()) return false;
    const auto time = snapshot->tstates;
    if (WaitForSingleObject(runner->notificationEvent(), 50) != WAIT_TIMEOUT
        || runner->takeSnapshot().tstates != time) return false;
    runner->load(request(fixture(root)));
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Running && s.frames >= 3; });
    runner->stop();
    return checker(*snapshot, 255, 0);
}

bool stopOutstanding(const std::filesystem::path& root) {
    const auto path = fixture(root);
    for (const bool load : {false, true}) {
        smsgg::test::FakeAudioConsumer audio;
        auto runner = std::make_unique<EmulatorRunner>(audio.stream);
        if (load) runner->load(request(path));
        runner->stop();
        if (runner->takeSnapshot().state != RunnerState::Stopped) return false;
    }
    return true;
}

bool stopDuringIo(const std::filesystem::path& root) {
    const auto path = fixture(root);
    smsgg::test::FakeAudioConsumer audio;
    auto runner = std::make_unique<EmulatorRunner>(audio.stream);
    auto snapshot = std::make_unique<RunnerSnapshot>();
    OVERLAPPED operation{};
    std::unique_ptr<void, decltype(&CloseHandle)> broken(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
    if (!broken) throw std::runtime_error("Oplock event creation failed");
    const auto rawFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (rawFile == INVALID_HANDLE_VALUE) throw std::runtime_error("Oplock file open failed");
    // Unwind closes the held file before the event and before runner's destructor joins.
    std::unique_ptr<void, decltype(&CloseHandle)> heldFile(rawFile, &CloseHandle);
    operation.hEvent = broken.get();
    DWORD bytes{};
    const auto granted = DeviceIoControl(heldFile.get(), FSCTL_REQUEST_OPLOCK_LEVEL_1,
        nullptr, 0, nullptr, 0, &bytes, &operation);
    if (granted || GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Oplock was not granted asynchronously");
    runner->load(request(path));
    waitFor(*runner, *snapshot, [](const auto& s) { return s.state == RunnerState::Loading; });
    if (WaitForSingleObject(broken.get(), 5000) != WAIT_OBJECT_0
        || !GetOverlappedResult(heldFile.get(), &operation, &bytes, FALSE)) throw std::runtime_error("Oplock break was not observed");
    *snapshot = runner->takeSnapshot();
    if (snapshot->state != RunnerState::Loading || snapshot->tstates || !snapshot->error.empty()) return false;
    smsgg::win32::RunnerTestAccess::requestStop(*runner);
    heldFile.reset(); // File I/O completes only after stop is requested.
    runner->stop();
    *snapshot = runner->takeSnapshot();
    return snapshot->state == RunnerState::Stopped && snapshot->romPath.empty()
        && snapshot->tstates == 0 && snapshot->frames == 0;
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const std::string_view name = argv[1];
    const std::filesystem::path root = argv[2];
    const bool pass = name == "lifecycle" ? lifecycle(root) : name == "loading" ? loading(root)
        : name == "input_frames" ? inputAndFrames(root) : name == "rapid_fire" ? rapidFire(root) : name == "fault" ? fault(root)
        : name == "stop" ? stopOutstanding(root) : name == "stop_io" && stopDuringIo(root);
    if (!pass) std::cerr << "Runner test failed: " << name << '\n';
    return pass ? 0 : 1;
}
