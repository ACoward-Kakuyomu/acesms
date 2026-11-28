#pragma once

#include "core/Machine.h"

#include <array>
#include <cstdint>
#include <span>

namespace smsgg {
enum class VideoStandard { Ntsc, Pal };
struct Rgba8 { uint8_t r{}, g{}, b{}, a{255}; };
struct DisplayArea { unsigned x, y, width, height; };

struct CramChange {
    uint16_t dot{};
    uint8_t index{};
    Rgba8 previous{};
};
inline constexpr unsigned maxCramChanges = 21; // At most one CPU write every 11 T-states in a 228 T-state line.
constexpr unsigned cramWriteDot(uint64_t tstate) {
    const auto dot = static_cast<unsigned>((3 * (tstate % 228) + 1) / 2);
    return dot < 256 ? dot : 256;
}

struct VdpState {
    static constexpr uint8_t frameFlagDiff = 0x80; // SMS spriteStatus bit in the state format.
    std::array<uint8_t,0x4000> vram{};
    uint16_t address{};
    uint8_t code{},buffer{};
    bool secondByte{};
    uint16_t line{};
    uint64_t cycle{},frames{};
    uint8_t hLatch{};
    std::array<uint8_t,11> registers{};
    bool framePending{},linePending{};
    uint8_t lineCounter{0xff};
    std::array<Rgba8,32> palette{};
    uint8_t cramLatch{};
    std::array<std::array<Rgba8,256*192>,2> images{};
    uint8_t drawing{1},scrollY{},spriteStatus{};
    uint8_t scrollX{},nextScrollX{};
    std::array<CramChange, maxCramChanges> cramChanges{};
    uint8_t cramChangeCount{};
};

class Vdp final {
public:
    enum class Startup { Reset, Cartridge };
    explicit Vdp(VideoStandard standard, Machine machine = Machine::Sms, Startup startup = Startup::Reset);
    void advanceTo(uint64_t targetTstate);
    uint8_t readVCounter(uint64_t tstate = 0);
    uint8_t readHCounter() const { return hLatch_; }
    void latchHCounter();
    uint64_t completedFrames() const { return frames_; }
    std::span<const Rgba8, 256 * 192> framePixels() const { return images_[drawing_ ^ 1]; }
    uint8_t readData();
    uint8_t readStatus(uint64_t tstate = 0);
    bool interruptPending() const;
    void writeData(uint8_t value) { writeData(value, cycle_); }
    void writeData(uint8_t value, uint64_t tstate);
    void writeControl(uint8_t value, uint64_t tstate = 0);
    // Caller owns storage for the two frame images; no frame-sized stack temporary.
    void captureState(VdpState& out) const noexcept;
    // Same validated machine/video profile. No port side effects or image regeneration.
    void restoreState(const VdpState& state) noexcept;
private:
    void clockLineCounter(bool active);
    uint16_t vramAddress() const;
    const Machine machine_;
    std::array<uint8_t, 0x4000> vram_{};
    uint16_t address_{};
    uint8_t code_{};
    uint8_t buffer_{};
    bool secondByte_{};
    unsigned linesPerFrame_;
    unsigned line_{};
    uint64_t cycle_{};
    uint64_t frames_{};
    uint8_t hLatch_{};
    std::array<uint8_t, 11> registers_{};
    bool framePending_{};
    bool frameFlag_{};
    bool linePending_{};
    uint8_t lineCounter_{0xff};
    std::array<Rgba8, 32> palette_{};
    uint8_t cramLatch_{};
    std::array<std::array<Rgba8, 256 * 192>, 2> images_{};
    unsigned drawing_{1};
    uint8_t scrollY_{};
    uint8_t scrollX_{};
    uint8_t nextScrollX_{};
    uint8_t spriteStatus_{};
    std::array<CramChange, maxCramChanges> cramChanges_{};
    uint8_t cramChangeCount_{};
};
} // namespace smsgg
