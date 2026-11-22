#include "core/video/Vdp.h"
#include "core/video/Mode4Renderer.h"
#include "core/video/Tms9918Renderer.h"

#include <algorithm>
#include <cassert>

namespace smsgg {
Vdp::Vdp(VideoStandard standard, Machine machine)
    : machine_(machine), linesPerFrame_(standard == VideoStandard::Ntsc ? 262 : 313) {
    registers_[10] = 0xff;
}

void Vdp::captureState(VdpState& out) const noexcept {
    out.vram=vram_;out.address=address_;out.code=code_;out.buffer=buffer_;out.secondByte=secondByte_;
    out.line=static_cast<uint16_t>(line_);out.cycle=cycle_;out.frames=frames_;out.hLatch=hLatch_;
    out.registers=registers_;out.framePending=framePending_;out.linePending=linePending_;out.lineCounter=lineCounter_;
    out.palette=palette_;out.cramLatch=cramLatch_;out.images=images_;
    out.drawing=static_cast<uint8_t>(drawing_);out.scrollY=scrollY_;out.spriteStatus=spriteStatus_;
    out.cramChanges=cramChanges_;out.cramChangeCount=cramChangeCount_;
}

void Vdp::restoreState(const VdpState& state) noexcept {
    vram_=state.vram;address_=state.address;code_=state.code;buffer_=state.buffer;secondByte_=state.secondByte;
    line_=state.line;cycle_=state.cycle;frames_=state.frames;hLatch_=state.hLatch;
    registers_=state.registers;framePending_=state.framePending;linePending_=state.linePending;lineCounter_=state.lineCounter;
    palette_=state.palette;cramLatch_=state.cramLatch;images_=state.images;
    drawing_=state.drawing;scrollY_=state.scrollY;spriteStatus_=state.spriteStatus;
    cramChanges_=state.cramChanges;cramChangeCount_=state.cramChangeCount;
}

void Vdp::advanceTo(uint64_t targetTstate) {
    const bool sg1000 = machine_ == Machine::Sg1000;
    while (cycle_ < targetTstate) {
        cycle_ += std::min<uint64_t>(targetTstate - cycle_, 228 - cycle_ % 228);
        if (cycle_ % 228 == 0) {
            if (sg1000) {
                if (line_ < 192) {
                    const auto status = Tms9918Renderer::renderLine(vram_, registers_, line_,
                        std::span<Rgba8, 256>(images_[drawing_].data() + line_ * 256, 256));
                    spriteStatus_ |= status & 0x20;
                    if (!framePending_ && !(spriteStatus_ & 0x40) && (status & 0x40)) spriteStatus_ |= status;
                }
                if (line_ == 191) framePending_ = true;
            } else {
                if (line_ < 192) spriteStatus_ |= Mode4Renderer::renderLine(vram_, palette_, registers_, scrollY_, line_,
                    std::span<Rgba8, 256>(images_[drawing_].data() + line_ * 256, 256),
                    std::span(cramChanges_).first(cramChangeCount_));
                cramChangeCount_ = 0;
                if (line_ <= 192) {
                    if (lineCounter_ == 0) { linePending_ = true; lineCounter_ = registers_[10]; }
                    else --lineCounter_;
                } else lineCounter_ = registers_[10];
                if (line_ == 192) framePending_ = true;
            }
            if (++line_ == linesPerFrame_) { line_ = 0; ++frames_; drawing_ ^= 1; scrollY_ = registers_[9]; }
        }
    }
}

uint8_t Vdp::readVCounter() const {
    if (linesPerFrame_ == 262) return static_cast<uint8_t>(line_ <= 218 ? line_ : line_ - 6);
    return static_cast<uint8_t>(line_ <= 242 ? line_ : line_ - 57);
}

void Vdp::latchHCounter() {
    const auto count = (cycle_ % 228) * 3 / 4;
    hLatch_ = static_cast<uint8_t>(count <= 0x93 ? count : count + 0x55);
}

bool Vdp::interruptPending() const {
    return (framePending_ && (registers_[1] & 0x20) != 0) || (linePending_ && (registers_[0] & 0x10) != 0);
}

uint8_t Vdp::readStatus() {
    secondByte_ = false;
    const uint8_t index = machine_ == Machine::Sg1000 && (spriteStatus_ & 0x40) ? 0 : 0x1f;
    const uint8_t status = static_cast<uint8_t>(index | spriteStatus_ | (framePending_ ? 0x80 : 0));
    framePending_ = false;
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

void Vdp::writeControl(uint8_t value) {
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
            registers_[value & 15] = static_cast<uint8_t>(address_);
        }
    }
}

uint16_t Vdp::vramAddress() const {
    return machine_ == Machine::Sg1000 ? Tms9918Renderer::vramAddress(address_, registers_[1]) : address_;
}
} // namespace smsgg
