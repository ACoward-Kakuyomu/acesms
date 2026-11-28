#include "core/video/Vdp.h"
#include "core/video/Mode4Renderer.h"
#include "core/video/Tms9918Renderer.h"

#include <cassert>

namespace smsgg {
namespace {
// First CPU instruction boundary that can see a new HINT through Z80 interrupt sampling.
constexpr unsigned smsLineCounterPhase = 213;
constexpr unsigned smsFrameIrqPhase = 212;
constexpr unsigned smsScrollLatchPhase = 211;
}

Vdp::Vdp(VideoStandard standard, Machine machine, Startup startup)
    : machine_(machine), linesPerFrame_(standard == VideoStandard::Ntsc ? 262 : 313) {
    registers_[10] = 0xff;
    if (machine_ == Machine::Sms && startup == Startup::Cartridge) {
        registers_[0] = 0x36; // Mode 4 at the BIOS handoff.
        registers_[2] = 0xff; // Background table at $3800.
        registers_[5] = 0xff; // Sprite attribute table at $3f00.
    }
}

void Vdp::captureState(VdpState& out) const noexcept {
    out.vram=vram_;out.address=address_;out.code=code_;out.buffer=buffer_;out.secondByte=secondByte_;
    out.line=static_cast<uint16_t>(line_);out.cycle=cycle_;out.frames=frames_;out.hLatch=hLatch_;
    out.registers=registers_;out.framePending=framePending_;out.linePending=linePending_;out.lineCounter=lineCounter_;
    out.palette=palette_;out.cramLatch=cramLatch_;out.images=images_;
    out.drawing=static_cast<uint8_t>(drawing_);out.scrollY=scrollY_;
    out.scrollX=scrollX_;out.nextScrollX=nextScrollX_;
    out.spriteStatus = spriteStatus_ | (frameFlag_ != framePending_ ? VdpState::frameFlagDiff : 0);
    out.cramChanges=cramChanges_;out.cramChangeCount=cramChangeCount_;
}

void Vdp::restoreState(const VdpState& state) noexcept {
    vram_=state.vram;address_=state.address;code_=state.code;buffer_=state.buffer;secondByte_=state.secondByte;
    line_=state.line;cycle_=state.cycle;frames_=state.frames;hLatch_=state.hLatch;
    registers_=state.registers;framePending_=state.framePending;linePending_=state.linePending;lineCounter_=state.lineCounter;
    palette_=state.palette;cramLatch_=state.cramLatch;images_=state.images;
    drawing_=state.drawing;scrollY_=state.scrollY;
    scrollX_=state.scrollX;nextScrollX_=state.nextScrollX;
    frameFlag_ = state.framePending != ((state.spriteStatus & VdpState::frameFlagDiff) != 0);
    spriteStatus_ = state.spriteStatus & ~VdpState::frameFlagDiff;
    cramChanges_=state.cramChanges;cramChangeCount_=state.cramChangeCount;
}

void Vdp::clockLineCounter(bool active) {
    if (active && lineCounter_ != 0) {
        --lineCounter_;
        return;
    }
    if (active) linePending_ = true;
    lineCounter_ = registers_[10];
}

void Vdp::advanceTo(uint64_t targetTstate) {
    const bool sms = machine_ == Machine::Sms;
    const bool sg1000 = machine_ == Machine::Sg1000;
    while (cycle_ < targetTstate) {
        const auto phase = cycle_ % 228;
        const bool scrollFirst = sms && phase < smsScrollLatchPhase;
        const bool frameIrqFirst = sms && line_ == 192 && phase < smsFrameIrqPhase;
        const bool counterFirst = sms && phase < smsLineCounterPhase;
        const auto toBoundary = (scrollFirst ? smsScrollLatchPhase : frameIrqFirst ? smsFrameIrqPhase :
            counterFirst ? smsLineCounterPhase : 228) - phase;
        if (targetTstate - cycle_ < toBoundary) {
            cycle_ = targetTstate;
            break;
        }
        cycle_ += toBoundary;
        if (scrollFirst) {
            nextScrollX_ = registers_[8];
        } else if (frameIrqFirst) {
            framePending_ = true;
        } else if (counterFirst) {
            // Count for the upcoming line, including line 0 after the top border.
            clockLineCounter(line_ < 192 || line_ + 1 == linesPerFrame_);
            if (line_ == 192) frameFlag_ = true;
        } else {
            if (sg1000) {
                if (line_ < 192) {
                    const auto status = Tms9918Renderer::renderLine(vram_, registers_, line_,
                        std::span<Rgba8, 256>(images_[drawing_].data() + line_ * 256, 256));
                    spriteStatus_ |= status & 0x20;
                    if (!framePending_ && !(spriteStatus_ & 0x40) && (status & 0x40)) spriteStatus_ |= status;
                }
                if (line_ == 191) framePending_ = frameFlag_ = true;
            } else {
                const auto nextLine = line_ + 1 == linesPerFrame_ ? 0 : line_ + 1;
                if (sms && nextLine < 192 && Mode4Renderer::spriteOverflow(vram_, registers_, nextLine))
                    spriteStatus_ |= 0x40;
                if (line_ < 192) spriteStatus_ |= Mode4Renderer::renderLine(vram_, palette_, registers_,
                    sms ? scrollX_ : registers_[8], scrollY_, line_,
                    std::span<Rgba8, 256>(images_[drawing_].data() + line_ * 256, 256),
                    std::span(cramChanges_).first(cramChangeCount_)) & (sms ? 0x20 : 0x60);
                cramChangeCount_ = 0;
                if (sms) scrollX_ = nextScrollX_;
                if (!sms) {
                    clockLineCounter(line_ <= 192);
                    if (line_ == 192) framePending_ = frameFlag_ = true;
                }
            }
            if (++line_ == linesPerFrame_) { line_ = 0; ++frames_; drawing_ ^= 1; scrollY_ = registers_[9]; }
        }
    }
}

uint8_t Vdp::readVCounter(uint64_t tstate) {
    if (machine_ == Machine::Sms) advanceTo(tstate);
    // H=$F4 is 16 T-states before the drawing line boundary on SMS.
    const auto line = machine_ == Machine::Sms && cycle_ % 228 >= 212
        ? (line_ + 1) % linesPerFrame_ : line_;
    if (linesPerFrame_ == 262) return static_cast<uint8_t>(line <= 218 ? line : line - 6);
    return static_cast<uint8_t>(line <= 242 ? line : line - 57);
}

void Vdp::latchHCounter() {
    // SMS first samples H=0 one dot (two master clocks) into that count.
    const auto sampleOffset = machine_ == Machine::Sms ? 2 : 0;
    const auto count = ((cycle_ % 228) * 3 + sampleOffset) / 4;
    hLatch_ = static_cast<uint8_t>(count <= 0x93 ? count : count + 0x55);
}

bool Vdp::interruptPending() const {
    return (framePending_ && (registers_[1] & 0x20) != 0) || (linePending_ && (registers_[0] & 0x10) != 0);
}

uint8_t Vdp::readStatus(uint64_t tstate) {
    if (machine_ == Machine::Sms) advanceTo(tstate);
    secondByte_ = false;
    const uint8_t index = machine_ == Machine::Sg1000 && (spriteStatus_ & 0x40) ? 0 : 0x1f;
    const uint8_t status = static_cast<uint8_t>(index | spriteStatus_ | (frameFlag_ ? 0x80 : 0));
    framePending_ = frameFlag_ = false;
    linePending_ = false;
    spriteStatus_ = 0;
    return status;
}

uint8_t Vdp::readData() {
    secondByte_ = false;
    const auto value = buffer_;
    buffer_ = vram_[vramAddress()];
    address_ = (address_ + 1) & 0x3fff;
    return value;
}

void Vdp::writeData(uint8_t value, uint64_t tstate) {
    secondByte_ = false;
    if (code_ == 3 && machine_ != Machine::Sg1000) {
        if (machine_ == Machine::GameGear) {
            if ((address_ & 1) == 0) cramLatch_ = value;
            else palette_[(address_ >> 1) & 31] = {static_cast<uint8_t>((cramLatch_ & 15) * 17),
                static_cast<uint8_t>((cramLatch_ >> 4) * 17), static_cast<uint8_t>((value & 15) * 17), 255};
        } else {
            advanceTo(tstate);
            const auto index = static_cast<uint8_t>(address_ & 31);
            const auto dot = cramWriteDot(cycle_);
            if (line_ < 192 && dot != 0) {
                assert(cramChangeCount_ < cramChanges_.size());
                cramChanges_[cramChangeCount_++] = {static_cast<uint16_t>(dot), index, palette_[index]};
            }
            palette_[index] = {static_cast<uint8_t>((value & 3) * 85),
                static_cast<uint8_t>(((value >> 2) & 3) * 85), static_cast<uint8_t>(((value >> 4) & 3) * 85), 255};
        }
    }
    else vram_[vramAddress()] = value;
    buffer_ = value;
    address_ = (address_ + 1) & 0x3fff;
}

void Vdp::writeControl(uint8_t value, uint64_t tstate) {
    if (!secondByte_) {
        address_ = static_cast<uint16_t>((address_ & 0x3f00) | value);
        secondByte_ = true;
    } else {
        address_ = static_cast<uint16_t>((address_ & 0xff) | ((value & 0x3f) << 8));
        code_ = value >> 6;
        secondByte_ = false;
        if (code_ == 0) {
            buffer_ = vram_[vramAddress()];
            address_ = (address_ + 1) & 0x3fff;
        } else if (machine_ == Machine::Sg1000) {
            if (value & 0x80) registers_[value & 7] = static_cast<uint8_t>(address_);
        } else if (code_ == 2 && (value & 15) < registers_.size()) {
            const auto reg = value & 15;
            if (machine_ == Machine::Sms && (reg == 1 || reg == 8)) advanceTo(tstate);
            registers_[reg] = static_cast<uint8_t>(address_);
        }
    }
}

uint16_t Vdp::vramAddress() const {
    return machine_ == Machine::Sg1000 ? Tms9918Renderer::vramAddress(address_, registers_[1]) : address_;
}
} // namespace smsgg
