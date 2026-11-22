#include "core/EmulatorSystem.h"

#include <array>
#include <utility>

namespace smsgg {
EmulatorSystem::EmulatorSystem(RomImage image, VideoStandard standard, uint32_t sampleRate, Machine machine, bool fmEnabled,FmInterpolation interpolation,FmBackendPtr backend)
    : machine_(machine), cartridge_(std::move(image)), memory_(cartridge_,machine), vdp_(standard,machine),
      audio_(cpuClockHz(standard),sampleRate,fmEnabled && machine==Machine::Sms && standard==VideoStandard::Ntsc,interpolation,std::move(backend),machine),
      smsIo_(controller_,audio_,machine,audio_.hasFm()), vdpIo_(vdp_, smsIo_), memoryControl_(memory_, vdpIo_,machine), io_(memoryControl_,machine,audio_.hasFm()),
      cpu_({.sp = 0xdff8}),
      audioBuffer_((maximumStepTstates*sampleRate+cpuClockHz(standard)-1)/cpuClockHz(standard)+1) {}

std::vector<uint8_t> EmulatorSystem::captureWorkRam() const {
    const auto ram = memory_.workRam();
    return {ram.begin(), ram.end()};
}

bool EmulatorSystem::writeWorkRam(unsigned address, SearchWidth width, uint32_t value) {
    if (!validWorkRamWrite(machine_, address, width, value)) return false;
    memory_.writeWorkRam(address - workRamBase, static_cast<unsigned>(width), value);
    return true;
}

void EmulatorSystem::captureState(EmulatorSystemState& out) const {
    out.tstates=tstates_;out.cpu=cpu_.captureState();out.memory=memory_.captureState();
    out.memoryControl=memoryControl_.captureState();out.cartridge=cartridge_.captureState();
    out.controller=controller_.captureState();vdp_.captureState(out.vdp);out.audio=audio_.captureState();
}
acesms_fm_result EmulatorSystem::restoreState(const EmulatorSystemState& state) {
    const auto result=audio_.restoreState(state.audio);
    if(result!=ACESMS_FM_OK) return result;
    cartridge_.restoreState(state.cartridge);memory_.restoreState(state.memory);
    memoryControl_.restoreState(state.memoryControl);controller_.restoreState(state.controller);
    vdp_.restoreState(state.vdp);cpu_.restoreState(state.cpu);
    tstates_=state.tstates;audioCount_=0;
    return ACESMS_FM_OK;
}
std::variant<uint32_t, CpuStepError> EmulatorSystem::step(PadState pad, bool consolePause, PadState player2) {
    audioCount_=0;
    controller_.setState(pad,hasSecondPad(machine_)?player2:PadState{});
    std::variant<uint32_t, CpuStepError> result;
    if (consolePause && machine_!=Machine::GameGear) result = cpu_.acceptNmi(memory_);
    else if (vdp_.interruptPending() && cpu_.canAcceptMaskableInterrupt()) {
        constexpr std::array<uint8_t, 1> acknowledge{0xff};
        result = *cpu_.acceptInterrupt(acknowledge, &memory_, &io_, tstates_);
    } else result = cpu_.step(memory_, io_, tstates_);
    if (const auto elapsed = std::get_if<uint32_t>(&result)) {
        tstates_ += *elapsed;
        vdp_.advanceTo(tstates_);
        audioCount_=audio_.advanceTo(tstates_,audioBuffer_);
    }
    return result;
}
} // namespace smsgg
