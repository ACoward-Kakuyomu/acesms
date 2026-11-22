#pragma once

#include "core/Machine.h"
#include "core/audio/FmResampler.h"

#include "core/bus/IoBus.h"
#include "core/bus/MemoryControl.h"
#include "core/bus/SmsIo.h"
#include "core/bus/VdpIo.h"
#include "core/cartridge/Cartridge.h"
#include "core/cheat/MemorySearch.h"
#include "core/video/Vdp.h"

#include <vector>

namespace smsgg {
constexpr uint32_t cpuClockHz(VideoStandard standard) { return standard==VideoStandard::Ntsc ? 3579545 : 3546893; }
struct EmulatorSystemState {
    uint64_t tstates{};
    Z80State cpu;
    MemoryBusState memory;
    MemoryControlState memoryControl;
    CartridgeState cartridge;
    ControllerState controller;
    VdpState vdp;
    AudioMixerState audio;
};
// Owned and called by one emulation thread; no host clock or window dependencies.
class EmulatorSystem final {
public:
    static constexpr uint64_t maximumStepTstates=4*65535+64;
    EmulatorSystem(RomImage image, VideoStandard standard, uint32_t sampleRate, Machine machine = Machine::Sms, bool fmEnabled = false,
        FmInterpolation interpolation=FmInterpolation::Sinc,FmBackendPtr backend={});
    EmulatorSystem(const EmulatorSystem&) = delete;
    EmulatorSystem& operator=(const EmulatorSystem&) = delete;
    EmulatorSystem(EmulatorSystem&&) = delete;
    EmulatorSystem& operator=(EmulatorSystem&&) = delete;
    std::variant<uint32_t, CpuStepError> step(PadState pad = {}, bool consolePause = false, PadState player2 = {});
    uint64_t tstates() const { return tstates_; }
    uint64_t completedFrames() const { return vdp_.completedFrames(); }
    DisplayArea displayArea() const { return machine_==Machine::GameGear ? DisplayArea{48,24,160,144} : DisplayArea{0,0,256,192}; }
    std::span<const Rgba8, 256 * 192> framePixels() const { return vdp_.framePixels(); }
    const Z80& cpu() const { return cpu_; }
    uint8_t readMemory(uint16_t address) { return memory_.read8(address); }
    std::vector<uint8_t> captureWorkRam() const;
    bool writeWorkRam(unsigned address, SearchWidth width, uint32_t value);
    std::span<uint8_t> cartridgeRam() noexcept { return cartridge_.ram(); }
    std::span<const AudioFrame> audioFrames() const { return std::span(audioBuffer_).first(audioCount_); }
    void captureState(EmulatorSystemState& out) const;
    // Restore captured/validated values into an independent candidate of the same ROM/profile.
    acesms_fm_result restoreState(const EmulatorSystemState& state);
private:
    const Machine machine_;
    Cartridge cartridge_;
    MemoryBus memory_;
    Controller controller_;
    Vdp vdp_;
    AudioMixer audio_;
    SmsIo smsIo_;
    VdpIo vdpIo_;
    MemoryControl memoryControl_;
    IoBus io_;
    Z80 cpu_;
    uint64_t tstates_{};
    std::vector<AudioFrame> audioBuffer_;
    size_t audioCount_{};
};
} // namespace smsgg
