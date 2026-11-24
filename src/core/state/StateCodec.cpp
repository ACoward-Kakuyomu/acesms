#include "core/state/StateCodec.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>

namespace smsgg {
namespace {
constexpr std::array<uint8_t,8> magic{'A','C','E','S','M','S','S','T'};
constexpr uint16_t stateVersion=1;
constexpr size_t cramHistoryBytes = 1 + maxCramChanges * 7; // count + (dot, index, RGBA)
uint8_t mapperId(MapperKind kind) {
    switch(kind) {case MapperKind::RomOnly:return 0;case MapperKind::Sega:return 1;case MapperKind::Codemasters:return 2;default:assert(false);return 255;}
}
uint8_t interpolationId(FmInterpolation method) {
    switch(method) {case FmInterpolation::Sinc:return 0;case FmInterpolation::ZohLowPass:return 1;case FmInterpolation::Linear:return 2;}
    assert(false);return 255;
}
size_t fmBytes(const StateProfile& p) {return p.fm?67+size_t(p.fm->backendBytes)+FmResampler::stateBytes(p.fm->method):0;}
uint32_t checksum(std::span<const uint8_t> bytes) {
    uint32_t crc=~0u;
    for(size_t n=0;n<bytes.size();++n) {
        crc^=n>=14 && n<18?0:bytes[n];
        for(unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
struct Writer {
    std::span<uint8_t> bytes;size_t at{};
    void integer(uint64_t v,unsigned width) {assert(at+width<=bytes.size());for(unsigned n=0;n<width;++n) {bytes[at++]=static_cast<uint8_t>(v);v>>=8;}}
    void u8(uint8_t v) {integer(v,1);}void u16(uint16_t v) {integer(v,2);}void u32(uint32_t v) {integer(v,4);}void u64(uint64_t v) {integer(v,8);}
    void real(double v) {u64(std::bit_cast<uint64_t>(v));}
    void block(std::span<const uint8_t> v) {assert(at+v.size()<=bytes.size());std::copy(v.begin(),v.end(),bytes.begin()+at);at+=v.size();}
};
struct Reader {
    std::span<const uint8_t> bytes;size_t at{};bool valid{true};
    uint64_t integer(unsigned width) {assert(at+width<=bytes.size());uint64_t v=0;for(unsigned n=0;n<width;++n) v|=uint64_t(bytes[at++])<<(n*8);return v;}
    uint8_t u8() {return static_cast<uint8_t>(integer(1));}uint16_t u16() {return static_cast<uint16_t>(integer(2));}
    uint32_t u32() {return static_cast<uint32_t>(integer(4));}uint64_t u64() {return integer(8);}
    bool boolean() {const auto v=u8();valid&=v<=1;return v==1;}
    double real() {return std::bit_cast<double>(u64());}
    std::span<const uint8_t> block(size_t length) {assert(at+length<=bytes.size());const auto v=bytes.subspan(at,length);at+=length;return v;}
};
bool validSg1000Profile(const StateProfile& profile, bool fm) {
    return profile.video == VideoStandard::Ntsc && profile.mapper == MapperKind::RomOnly
        && profile.ramBytes == 0 && !fm && supportsLinearRomSize(profile.romBytes, profile.machine);
}
bool validValues(const EmulatorSystemState& s,const StateProfile& p) {
    const auto& v=s.vdp;const auto& a=s.audio;const auto& g=a.psg;
    const bool sg1000 = p.machine == Machine::Sg1000;
    const uint64_t lines=p.video==VideoStandard::Ntsc?262:313;
    const uint64_t clock=cpuClockHz(p.video),phase=(s.tstates%clock)*p.sampleRate%clock;
    if(s.tstates>std::numeric_limits<uint64_t>::max()-EmulatorSystem::maximumStepTstates || s.cpu.eiDelay>1 || s.cpu.interruptMode>2 ||
        v.address>=0x4000 || v.code>3 || v.cycle!=s.tstates || v.line!=(s.tstates/228)%lines || v.frames!=s.tstates/(228*lines) ||
        v.drawing!=(1^(v.frames&1)) || (v.hLatch>0x93 && v.hLatch<0xe9) ||
        a.time!=s.tstates || a.clock.phase!=phase || a.interval!=phase/p.sampleRate || !std::isfinite(a.area.left) || !std::isfinite(a.area.right) ||
        std::abs(a.area.left)>static_cast<double>(a.interval) || std::abs(a.area.right)>static_cast<double>(a.interval) || a.fmControl>3 || (!p.fm && a.fmControl)) return false;
    if(p.machine==Machine::GameGear && (!s.memory.cartridgeEnabled || s.memoryControl.ioDisabled)) return false;
    if(p.machine==Machine::Sms && v.cramLatch) return false;
    if (sg1000) {
        if (!s.memory.ramEnabled || !s.memory.cartridgeEnabled || s.memoryControl.ioDisabled
            || v.linePending || v.cramLatch || (v.spriteStatus & 0x80)
            || ((v.spriteStatus & 0x1f) && !(v.spriteStatus & 0x40))
            || g.lfsr > 0x7fff || g.stereo != 0xff || a.area.left != a.area.right) return false;
    } else if (v.spriteStatus & ~0x60) return false;
    const unsigned scale=p.machine==Machine::Sms?85:17;
    auto color=[&](Rgba8 c){return c.a==255 && (sg1000 || (c.r%scale==0 && c.g%scale==0 && c.b%scale==0));};
    for(auto c:v.palette) if(!color(c)) return false;
    if (v.cramChangeCount > v.cramChanges.size() ||
        (v.cramChangeCount && (p.machine != Machine::Sms || v.line >= 192))) return false;
    const auto lineCycle = v.cycle % 228;
    constexpr unsigned firstSaturatedCycle = 171;
    constexpr unsigned shortestOut = 11;
    unsigned nextWriteCycle = 1; // Writes at the start of the line are not recorded.
    for (const auto& change : std::span(v.cramChanges).first(v.cramChangeCount)) {
        // Saturated dots can occupy any available cycle in the horizontal blanking interval.
        const auto writeCycle = change.dot == 256 ? std::max(firstSaturatedCycle, nextWriteCycle) : change.dot * 2u / 3;
        if (writeCycle < nextWriteCycle || writeCycle > lineCycle || cramWriteDot(writeCycle) != change.dot ||
            change.index >= v.palette.size() || !color(change.previous))
            return false;
        nextWriteCycle = writeCycle + shortestOut;
    }
    for(const auto& image:v.images) for(auto c:image) if(!color(c)) return false;
    for(unsigned i=0;i<3;++i) if(g.tones[i]>1023 || !g.counters[i] || g.counters[i]>(sg1000?1024:1023)) return false;
    for(auto value:g.volumes) if(value>15) return false;
    if(g.selected>7 || g.noise>7 || g.divider!=s.tstates%16 || !g.noiseCounter || g.noiseCounter>(16u<<(g.noise&3)) || !g.lfsr) return false;
    if(p.mapper==MapperKind::Codemasters && std::get<CodemastersMapperState>(s.cartridge.mapper).banks[1]>127) return false;
    return true;
}
}
std::variant<StateProfile,StateCodecError> inspectStateProfile(std::span<const uint8_t> bytes) {
    using Error=StateCodecError;
    if(bytes.size()<42) return Error::Size;
    Reader r{bytes};const auto signature=r.block(8);
    if(!std::equal(signature.begin(),signature.end(),magic.begin())) return Error::Format;
    const auto version=r.u16();
    if(r.u32()!=bytes.size()-42) return Error::Size;
    r.u32();StateProfile p;p.romBytes=r.u32();p.romCrc32=r.u32();
    const auto machine=r.u8(),video=r.u8(),mapper=r.u8(),fm=r.u8();
    if(machine>static_cast<uint8_t>(Machine::Sg1000) || video>1 || mapper>2 || fm>1) return Error::Format;
    p.machine=static_cast<Machine>(machine);p.video=static_cast<VideoStandard>(video);p.mapper=static_cast<MapperKind>(mapper);
    if (version != stateVersion) return Error::Format;
    p.ramBytes=r.u32();p.sampleRate=r.u32();const auto length=r.u32();
    if((p.ramBytes!=0 && p.ramBytes!=8192 && p.ramBytes!=16384 && p.ramBytes!=32768) || !p.sampleRate || p.sampleRate>cpuClockHz(p.video) ||
        (p.machine == Machine::Sg1000 && !validSg1000Profile(p, fm != 0)) ||
        (p.machine==Machine::GameGear && (p.video!=VideoStandard::Ntsc || fm)) || (fm && p.video!=VideoStandard::Ntsc)) return Error::Profile;
    if(p.mapper==MapperKind::RomOnly) {
        if(p.ramBytes || !supportsLinearRomSize(p.romBytes, p.machine)) return Error::Profile;
    } else if(p.mapper==MapperKind::Sega) {
        if(!supportsSegaRomSize(p.romBytes, p.machine)) return Error::Profile;
    } else if(p.romBytes<32768 || p.romBytes>1048576 || !std::has_single_bit(p.romBytes) ||
        p.ramBytes>8192) return Error::Profile;
    const auto base=stateFileBytes(p);
    if(bytes.size()<base || length!=bytes.size()-base) return Error::Size;
    if(fm) {
        if(length<67) return Error::Size;
        r.at=base;FmStateProfile f;
        auto id=r.block(16);std::copy(id.begin(),id.end(),f.backendId.begin());f.stateVersion=r.u32();
        id=r.block(16);std::copy(id.begin(),id.end(),f.compatibilityId.begin());f.clock=r.u32();f.divider=r.u32();
        r.u64();r.u32();const auto method=r.u8();f.resamplerVersion=r.u16();f.backendBytes=r.u32();const auto resamplerBytes=r.u32();
        if(method>2 || f.resamplerVersion!=1 || f.clock!=3579545 || f.divider!=72 || !f.stateVersion || !f.backendBytes ||
            std::all_of(f.backendId.begin(),f.backendId.end(),[](auto b){return b==0;}) || std::all_of(f.compatibilityId.begin(),f.compatibilityId.end(),[](auto b){return b==0;})) return Error::Profile;
        f.method=static_cast<FmInterpolation>(method);p.fm=f;
        if(resamplerBytes!=FmResampler::stateBytes(f.method) || bytes.size()!=stateFileBytes(p)) return Error::Size;
    } else if(length) return Error::Size;
    return p;
}
size_t stateFileBytes(const StateProfile& p) {
    return 42+418086+size_t(p.ramBytes)+(p.mapper==MapperKind::RomOnly?0:4)+fmBytes(p)
        +(p.machine == Machine::Sms ? cramHistoryBytes : 0)+hasSecondPad(p.machine);
}
std::vector<uint8_t> encodeState(const StateProfile& p,const EmulatorSystemState& s) {
    assert(s.cartridge.ram.size()==p.ramBytes && s.audio.fm.has_value()==p.fm.has_value());
    std::vector<uint8_t> bytes(stateFileBytes(p));Writer w{bytes};
    w.block(magic);w.u16(stateVersion);w.u32(static_cast<uint32_t>(bytes.size()-42));w.u32(0);w.u32(p.romBytes);w.u32(p.romCrc32);
    w.u8(static_cast<uint8_t>(p.machine));w.u8(p.video==VideoStandard::Ntsc?0:1);w.u8(mapperId(p.mapper));w.u8(p.fm.has_value());
    w.u32(p.ramBytes);w.u32(p.sampleRate);w.u32(static_cast<uint32_t>(fmBytes(p)));w.u64(s.tstates);
    const auto& c=s.cpu;const auto& r=c.registers;
    for(auto value:{r.af,r.bc,r.de,r.hl,r.afAlternate,r.bcAlternate,r.deAlternate,r.hlAlternate,r.ix,r.iy,r.sp,r.pc}) w.u16(value);
    w.u8(r.i);w.u8(r.r);for(bool value:{c.iff1,c.iff2,c.halted,c.flagsModified,c.ldAir}) w.u8(value);
    w.u8(c.eiDelay);w.u8(c.interruptMode);w.u16(c.memptr);
    w.block(s.memory.ram);w.u8(s.memory.ramEnabled);w.u8(s.memory.cartridgeEnabled);w.u8(s.memoryControl.ioDisabled);w.block(s.cartridge.ram);
    if(p.mapper==MapperKind::Sega) {const auto& m=std::get<SegaMapperState>(s.cartridge.mapper);w.block(m.banks);w.u8(m.control);}
    if(p.mapper==MapperKind::Codemasters) {const auto& m=std::get<CodemastersMapperState>(s.cartridge.mapper);w.block(m.banks);w.u8(m.ramEnabled);}
    const auto& pad=s.controller.pad;
    for(bool value:{pad.up,pad.down,pad.left,pad.right,pad.button1,pad.button2,pad.start}) w.u8(value);
    w.u8(s.controller.control);
    if(hasSecondPad(p.machine)) w.u8(digitalPadBits(s.controller.player2));
    const auto& v=s.vdp;w.block(v.vram);w.u16(v.address);w.u8(v.code);w.u8(v.buffer);w.u8(v.secondByte);w.u16(v.line);w.u64(v.cycle);w.u64(v.frames);
    w.u8(v.hLatch);w.block(v.registers);w.u8(v.framePending);w.u8(v.linePending);w.u8(v.lineCounter);
    auto color=[&](Rgba8 pixel){w.u8(pixel.r);w.u8(pixel.g);w.u8(pixel.b);w.u8(pixel.a);};
    for(auto pixel:v.palette) color(pixel);
    w.u8(v.cramLatch);for(const auto& image:v.images) for(auto pixel:image) color(pixel);
    w.u8(v.drawing);w.u8(v.scrollY);w.u8(v.spriteStatus);
    const auto& a=s.audio;const auto& g=a.psg;
    for(auto value:g.tones) w.u16(value);w.block(g.volumes);for(auto value:g.counters) w.u16(value);
    for(bool value:g.outputs) w.u8(value);w.u8(g.selected);w.u8(g.noise);w.u8(g.stereo);w.u8(g.divider);w.u16(g.noiseCounter);w.u16(g.lfsr);w.u8(g.noisePhase);w.u8(g.noiseOutput);
    w.u64(a.clock.phase);w.u64(a.time);w.u64(a.interval);w.real(a.area.left);w.real(a.area.right);w.u8(a.fmControl);
    if (p.machine == Machine::Sms) {
        w.u8(v.cramChangeCount);
        for (size_t i = 0; i < v.cramChanges.size(); ++i) {
            if (i < v.cramChangeCount) {
                const auto& change = v.cramChanges[i];
                w.u16(change.dot);w.u8(change.index);color(change.previous);
            } else {
                w.u16(0);w.u8(0);w.u32(0);
            }
        }
    }
    if(p.fm) {
        const auto& f=*p.fm;assert(a.fm->backend.size()==f.backendBytes);
        w.block(f.backendId);w.u32(f.stateVersion);w.block(f.compatibilityId);w.u32(f.clock);w.u32(f.divider);w.u64(s.tstates);w.u32(static_cast<uint32_t>(s.tstates%72));
        w.u8(interpolationId(f.method));w.u16(f.resamplerVersion);w.u32(f.backendBytes);w.u32(static_cast<uint32_t>(FmResampler::stateBytes(f.method)));
        w.block(a.fm->backend);const auto count=FmResampler::stateBytes(f.method);FmResampler::encodeState(a.fm->resampler,f.method,w.bytes.subspan(w.at,count));w.at+=count;
    }
    assert(w.at==bytes.size());w.at=14;w.u32(checksum(bytes));return bytes;
}
namespace {
std::variant<std::unique_ptr<EmulatorSystemState>,StateCodecError> decodeStateWithRate(
    std::span<const uint8_t> bytes, StateProfile& p, bool useSavedRate) {
    using Error=StateCodecError;
    if(bytes.size()!=stateFileBytes(p)) return Error::Size;
    Reader rd{bytes};const auto signature=rd.block(8);
    const auto version=rd.u16();
    if(!std::equal(signature.begin(),signature.end(),magic.begin()) || version!=stateVersion) return Error::Format;
    const auto payload=rd.u32(),crc=rd.u32(),romBytes=rd.u32(),romCrc=rd.u32();
    const auto machine=rd.u8(),video=rd.u8(),mapper=rd.u8(),fm=rd.u8();const auto ram=rd.u32(),rate=rd.u32(),fmLength=rd.u32();
    if(machine>static_cast<uint8_t>(Machine::Sg1000) || video>1 || mapper>2 || fm>1) return Error::Format;
    if(payload!=bytes.size()-42) return Error::Size;
    if(crc!=checksum(bytes)) return Error::Checksum;
    if(romBytes!=p.romBytes || romCrc!=p.romCrc32) return Error::Identity;
    if(machine!=static_cast<uint8_t>(p.machine) || video!=(p.video==VideoStandard::Ntsc?0:1) ||
        mapper!=mapperId(p.mapper) || fm!=static_cast<uint8_t>(p.fm.has_value()) || ram!=p.ramBytes ||
        (!useSavedRate && rate!=p.sampleRate) || (p.machine == Machine::Sg1000 && !validSg1000Profile(p, fm != 0))) return Error::Profile;
    if(fmLength!=fmBytes(p)) return Error::Size;
    if (useSavedRate) {
        if (!rate || rate > cpuClockHz(p.video)) return Error::Value;
        p.sampleRate = rate;
    }
    auto state=std::make_unique<EmulatorSystemState>();auto& s=*state;s.tstates=rd.u64();auto& c=s.cpu;auto& r=c.registers;
    for(auto* value:{&r.af,&r.bc,&r.de,&r.hl,&r.afAlternate,&r.bcAlternate,&r.deAlternate,&r.hlAlternate,&r.ix,&r.iy,&r.sp,&r.pc}) *value=rd.u16();
    r.i=rd.u8();r.r=rd.u8();c.iff1=rd.boolean();c.iff2=rd.boolean();c.halted=rd.boolean();c.flagsModified=rd.boolean();c.ldAir=rd.boolean();
    c.eiDelay=rd.u8();c.interruptMode=rd.u8();c.memptr=rd.u16();
    auto block=[&](auto& destination){const auto input=rd.block(destination.size());std::copy(input.begin(),input.end(),destination.begin());};
    block(s.memory.ram);s.memory.ramEnabled=rd.boolean();s.memory.cartridgeEnabled=rd.boolean();s.memoryControl.ioDisabled=rd.boolean();
    s.cartridge.ram.resize(p.ramBytes);block(s.cartridge.ram);
    if(p.mapper==MapperKind::Sega) {SegaMapperState m;block(m.banks);m.control=rd.u8();s.cartridge.mapper=m;}
    if(p.mapper==MapperKind::Codemasters) {CodemastersMapperState m;block(m.banks);m.ramEnabled=rd.boolean();s.cartridge.mapper=m;}
    auto& pad=s.controller.pad;pad.up=rd.boolean();pad.down=rd.boolean();pad.left=rd.boolean();pad.right=rd.boolean();pad.button1=rd.boolean();pad.button2=rd.boolean();pad.start=rd.boolean();s.controller.control=rd.u8();
    if(hasSecondPad(p.machine)) {
        const auto bits=rd.u8();
        if(bits&0xc0) return Error::Value;
        s.controller.player2={bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8),bool(bits&16),bool(bits&32)};
    }
    auto& v=s.vdp;block(v.vram);v.address=rd.u16();v.code=rd.u8();v.buffer=rd.u8();v.secondByte=rd.boolean();v.line=rd.u16();v.cycle=rd.u64();v.frames=rd.u64();v.hLatch=rd.u8();block(v.registers);
    v.framePending=rd.boolean();v.linePending=rd.boolean();v.lineCounter=rd.u8();auto color=[&](){return Rgba8{rd.u8(),rd.u8(),rd.u8(),rd.u8()};};
    for(auto& pixel:v.palette) pixel=color();v.cramLatch=rd.u8();for(auto& image:v.images) for(auto& pixel:image) pixel=color();
    v.drawing=rd.u8();v.scrollY=rd.u8();v.spriteStatus=rd.u8();auto& a=s.audio;auto& g=a.psg;
    for(auto& value:g.tones) value=rd.u16();block(g.volumes);for(auto& value:g.counters) value=rd.u16();for(auto& value:g.outputs) value=rd.boolean();
    g.selected=rd.u8();g.noise=rd.u8();g.stereo=rd.u8();g.divider=rd.u8();g.noiseCounter=rd.u16();g.lfsr=rd.u16();g.noisePhase=rd.boolean();g.noiseOutput=rd.boolean();
    a.clock.phase=rd.u64();a.time=rd.u64();a.interval=rd.u64();a.area.left=rd.real();a.area.right=rd.real();a.fmControl=rd.u8();
    if (p.machine == Machine::Sms) {
        v.cramChangeCount = rd.u8();
        for (size_t i = 0; i < v.cramChanges.size(); ++i) {
            const CramChange change{rd.u16(), rd.u8(), color()};
            if (i < v.cramChangeCount) v.cramChanges[i] = change;
            else if (change.dot || change.index || change.previous.r || change.previous.g ||
                change.previous.b || change.previous.a) return Error::Value;
        }
    }
    if(!rd.valid || !validValues(s,p)) return Error::Value;
    if(p.fm) {
        const auto& f=*p.fm;const auto id=rd.block(16);const auto version=rd.u32();const auto compatibility=rd.block(16);const auto clock=rd.u32(),divider=rd.u32();
        const auto ticks=rd.u64();const auto phase=rd.u32();const auto method=rd.u8();const auto resamplerVersion=rd.u16();const auto backendLength=rd.u32(),resamplerLength=rd.u32();
        if(!std::equal(id.begin(),id.end(),f.backendId.begin()) || version!=f.stateVersion || !std::equal(compatibility.begin(),compatibility.end(),f.compatibilityId.begin()) ||
            clock!=f.clock || divider!=f.divider || method!=interpolationId(f.method) || resamplerVersion!=f.resamplerVersion) return Error::Profile;
        if(ticks!=s.tstates || phase!=ticks%72) return Error::Value;
        if(backendLength!=f.backendBytes || resamplerLength!=FmResampler::stateBytes(f.method)) return Error::Size;
        const auto backend=rd.block(f.backendBytes);const auto converted=FmResampler::decodeState(rd.block(resamplerLength),f.clock,p.sampleRate,f.method,ticks);
        if(!converted) return Error::Value;
        a.fm=FmAudioState{{backend.begin(),backend.end()},*converted};
    }
    assert(rd.at==bytes.size());return state;
}
}

std::variant<std::unique_ptr<EmulatorSystemState>,StateCodecError> decodeState(std::span<const uint8_t> bytes,const StateProfile& expected) {
    auto profile = expected;
    return decodeStateWithRate(bytes, profile, false);
}

std::variant<DecodedState,StateCodecError> decodeStateAtSavedRate(std::span<const uint8_t> bytes,StateProfile expected) {
    auto decoded = decodeStateWithRate(bytes, expected, true);
    if (auto* error = std::get_if<StateCodecError>(&decoded)) return *error;
    return DecodedState{expected, std::get<std::unique_ptr<EmulatorSystemState>>(std::move(decoded))};
}
}
