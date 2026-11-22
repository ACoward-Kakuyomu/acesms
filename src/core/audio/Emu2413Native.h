#pragma once

#include <array>
#include <cstdint>
#include <span>

struct __OPLL;

namespace smsgg {
struct Emu2413PatchState {
    uint32_t tl{},fb{},eg{},ml{},ar{},dr{},sl{},rr{},kr{},kl{},am{},pm{},ws{};
    bool operator==(const Emu2413PatchState&) const = default;
};
struct Emu2413SlotState {
    uint8_t number{},type{},patch{},wave{};
    std::array<int32_t,2> output{};
    uint32_t pgPhase{},pgOut{};
    uint8_t pgKeep{};
    uint16_t blkFnum{},fnum{};
    uint8_t blk{},egState{};
    int32_t volume{};
    uint8_t keyFlag{},susFlag{};
    uint16_t tll{};
    uint8_t rks{},egRateHigh{},egRateLow{};
    uint32_t egShift{},egOut{},updateRequests{};
    bool operator==(const Emu2413SlotState&) const = default;
};
struct Emu2413NativeState {
    uint64_t masterTicks{};
    uint8_t chipType{};
    uint32_t address{};
    std::array<uint8_t,64> registers{};
    uint8_t testFlag{};
    uint32_t keyStatus{};
    uint8_t rhythmMode{};
    uint32_t egCounter{},pmPhase{};
    int32_t amPhase{};
    uint8_t lfoAm{};
    uint32_t noise{};
    uint8_t shortNoise{};
    std::array<int32_t,9> patchNumbers{};
    std::array<Emu2413SlotState,18> slots{};
    std::array<Emu2413PatchState,38> patches{};
    std::array<uint8_t,16> pan{};
    std::array<std::array<int32_t,2>,16> panFine{};
    uint32_t mask{};
    std::array<int16_t,14> channelOutput{};
    std::array<int16_t,2> mixOutput{};
    bool operator==(const Emu2413NativeState&) const = default;
};
// Fixed emu2413, one native sample at every 72nd master tick. No host-rate SRC.
class Emu2413Native final {
public:
    Emu2413Native();
    ~Emu2413Native();
    Emu2413Native(const Emu2413Native&)=delete;
    Emu2413Native& operator=(const Emu2413Native&)=delete;
    void reset();
    uint64_t masterTicks() const noexcept { return time_; }
    // Internal caller supplies monotonic time and target/72 - masterTicks()/72 slots.
    size_t advanceTo(uint64_t target,std::span<int32_t> output);
    void writeIo(uint8_t addressOrData,uint8_t value);
    Emu2413NativeState captureState() const noexcept;
    // Captured/validated values for this fixed backend; external bytes are validated at their boundary.
    void restoreState(const Emu2413NativeState& state) noexcept;
    static constexpr uint32_t stateBytes=3193;
    // Save caller provides at least stateBytes. Load validates the complete external blob once.
    void saveState(std::span<uint8_t> bytes) const noexcept;
    bool loadState(std::span<const uint8_t> bytes,uint64_t expectedMasterTicks) noexcept;
private:
    __OPLL* chip_;
    std::array<uint16_t*,2> waves_{};
    uint64_t time_{};
};
} // namespace smsgg
