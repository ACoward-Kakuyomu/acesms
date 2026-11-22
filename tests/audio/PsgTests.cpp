#include "core/audio/Psg.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace smsgg {
struct PsgTestAccess {
    static auto tones(const Psg& p) { return p.tones_; }
    static auto volumes(const Psg& p) { return p.volumes_; }
    static uint8_t noise(const Psg& p) { return p.noise_; }
    static uint16_t lfsr(const Psg& p) { return p.lfsr_; }
};
}
namespace {
using smsgg::Psg;
using smsgg::PsgTestAccess;
bool near(double a, double b) { return std::abs(a-b) < 1e-6; }
bool near(smsgg::PsgArea a, smsgg::PsgArea b) { return near(a.left,b.left) && near(a.right,b.right); }
bool near(smsgg::PsgArea a, double b) { return near(a,smsgg::PsgArea{b,b}); }
bool registers() {
    Psg p;
    if (PsgTestAccess::tones(p) != std::array<uint16_t,3>{} || !near(p.advance(100),0)) return false;
    if (PsgTestAccess::volumes(p) != std::array<uint8_t,4>{15,15,15,15}) return false;
    for (unsigned channel=0; channel<3; ++channel) {
        p.write(static_cast<uint8_t>(0x80 | (channel<<5) | 0xe));
        p.write(0x4f); // Bit 6 ignored: 0x0f high six bits.
        if (PsgTestAccess::tones(p)[channel] != 0xfe) return false;
        p.write(static_cast<uint8_t>(0x80 | (channel<<5) | 1));
        if (PsgTestAccess::tones(p)[channel] != 0xf1) return false;
        p.write(0x3f);
        if (PsgTestAccess::tones(p)[channel] != 0x3f1) return false;
    }
    for (unsigned ch=0; ch<4; ++ch) {
        p.write(static_cast<uint8_t>(0x90 | (ch<<5) | 0xa));
        p.write(0x73);
        if (PsgTestAccess::volumes(p)[ch] != 3) return false;
    }
    p.write(0xef); p.write(0x74);
    return PsgTestAccess::noise(p)==4 && PsgTestAccess::lfsr(p)==0x8000;
}
bool tone() {
    for (const unsigned n : {0u,1u,2u,1023u}) {
        Psg p;
        p.write(static_cast<uint8_t>(0x80|(n&15))); p.write(static_cast<uint8_t>(n>>4)); p.write(0x90);
        if (n<2) { if (!near(p.advance(32768),8192)) return false; }
        else {
            // Initial counter is 1. First transition at 16T, subsequent ones 16*N apart.
            if (!near(p.advance(15),-3.75) || !near(p.advance(1),-.25)
                || !near(p.advance(16*n),4.0*n) || !near(p.advance(16*n),-4.0*n)) return false;
        }
    }
    for (unsigned a=0; a<16; ++a) {
        Psg p; p.write(static_cast<uint8_t>(0x90|a));
        if (!near(p.advance(100), a==15 ? 0 : 25*std::pow(10.0,-double(a)/10))) return false;
    }
    Psg p; p.write(0x83); p.write(0); p.write(0x90); p.advance(16);
    p.write(0x82); // Do not reload the in-progress counter of 3.
    return near(p.advance(48),12) && near(p.advance(32),-8);
}
bool noise() {
    constexpr std::array<uint16_t,17> golden{0x8000,0x4000,0x2000,0x1000,0x0800,0x0400,
        0x0200,0x0100,0x0080,0x0040,0x0020,0x0010,0x0008,0x8004,0x4002,0x2001,0x9000};
    for (unsigned rate=0; rate<3; ++rate) {
        Psg p; p.write(static_cast<uint8_t>(0xe4|rate));
        const unsigned half=16*(16u<<rate);
        if (!near(p.advance(half-1),0) || PsgTestAccess::lfsr(p)!=golden[0]) return false;
        p.advance(1);
        for (unsigned shift=1; shift<golden.size(); ++shift) {
            if (PsgTestAccess::lfsr(p)!=golden[shift]) return false;
            p.advance(2*half);
        }
        p.write(static_cast<uint8_t>(rate)); // Latched noise DATA also resets.
        if (PsgTestAccess::lfsr(p)!=0x8000) return false;
        p.advance(half + 30*half); // 16 rising edges, periodic seed restored.
        if (PsgTestAccess::lfsr(p)!=0x8000) return false;
    }
    Psg p; p.write(0xc2); p.write(0); p.write(0xe7);
    p.advance(16); if (PsgTestAccess::lfsr(p)!=0x4000) return false;
    p.advance(63); if (PsgTestAccess::lfsr(p)!=0x4000) return false;
    p.advance(1); return PsgTestAccess::lfsr(p)==0x2000;
}
bool area() {
    Psg a,b;
    for (auto* p : {&a,&b}) { p->write(0x87); p->write(0); p->write(0x90); p->write(0xe4); p->write(0xf3); }
    const auto whole=a.advance(100003);
    smsgg::PsgArea parts; for (unsigned t=0;t<100003;++t) { const auto a=b.advance(1); parts.left+=a.left; parts.right+=a.right; }
    if (!near(whole,parts)) return false;
    // Muted channels keep their divider/LFSR position.
    a.write(0x9f); b.write(0x9f); a.advance(1234); b.advance(1234);
    a.write(0x90); b.write(0x90);
    if (!near(a.advance(4000),b.advance(4000))) return false;
    Psg p; p.write(0x90);
    return near(p.advance(10),2.5) && (p.write(0x9f), near(p.advance(10),0));
}

bool sg1000() {
    using smsgg::Machine;
    Psg zero(Machine::Sg1000);
    zero.write(0x90);
    if (!near(zero.advance(16), -4) || !near(zero.advance(16384), 4096)
        || !near(zero.advance(16384), -4096)) return false;
    Psg one(Machine::Sg1000);
    one.write(0x81); one.write(0x90);
    if (!near(one.advance(16), -4) || !near(one.advance(16), 4)
        || !near(one.advance(16), -4)) return false;

    Psg whole(Machine::Sg1000), split(Machine::Sg1000);
    for (auto* p : {&whole, &split}) { p->write(0xe4); p->write(0xf0); }
    constexpr std::array<uint16_t, 16> white{0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200,
        0x0100, 0x0080, 0x0040, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x4001, 0x6000};
    for (unsigned i = 0; i < white.size(); ++i) {
        if (PsgTestAccess::lfsr(whole) != white[i] || PsgTestAccess::lfsr(split) != white[i]) return false;
        if (i + 1 == white.size()) break;
        const unsigned ticks = i == 0 ? 256 : 512;
        const auto area = whole.advance(ticks);
        const auto first = split.advance(ticks - 7);
        const auto second = split.advance(7);
        if (!near(area, {first.left + second.left, first.right + second.right}) || !near(area.left, area.right))
            return false;
    }
    whole.write(0xe0);
    whole.advance(256 + 14 * 512);
    return PsgTestAccess::lfsr(whole) == 0x4000;
}
}
int main(int argc,char** argv) {
    const std::string_view name=argc==2?argv[1]:"";
    const bool pass=name=="registers"?registers():name=="tone"?tone():name=="noise"?noise():
        name=="sg1000"?sg1000():name=="area"&&area();
    if(!pass) std::cerr<<"PSG test failed: "<<name<<'\n';
    return pass?0:1;
}
