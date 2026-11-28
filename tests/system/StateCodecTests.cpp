#include "core/state/StateCodec.h"
#include "core/cartridge/RomLoader.h"
#include "Stage5TestRom.h"
#include "Sg1000TestRom.h"
#include "../cpu/Z80TestSupport.h"
#ifdef _WIN32
#include "win32/FmPlugin.h"
#endif

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string_view>

namespace {
using namespace smsgg;
struct Case {Machine machine;VideoStandard video;MapperKind mapper;uint32_t rate;bool fm;FmInterpolation method;};
RomImage image(MapperKind mapper,size_t ram=8192) {
    auto bytes=test::stage5InputRom();if(mapper!=MapperKind::RomOnly) bytes.resize(131072);
    bytes[1]=0;bytes[2]=8;size_t at=0x800;
    auto out=[&](uint8_t port,uint8_t value){for(auto b:{uint8_t(0x3e),value,uint8_t(0xd3),port}) bytes[at++]=b;};
    out(0x3f,0x55);out(0x7f,0x83);out(0x7f,1);out(0x7f,0x92);
    out(0xf2,3);out(0xf0,0x30);out(0xf1,0x10);out(0xf0,0x10);out(0xf1,0x80);out(0xf0,0x20);out(0xf1,0x19);
    bytes[at++]=0xc3;bytes[at++]=0;bytes[at]=1;
    return std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),{mapper,mapper==MapperKind::RomOnly?0:ram}));
}
StateProfile profile(const RomImage& rom,const Case& c,const FmBackendPtr& backend) {
    StateProfile p{static_cast<uint32_t>(rom.bytes().size()),rom.metadata().crc32,c.machine,c.video,c.mapper,static_cast<uint32_t>(rom.options().sramBytes),c.rate,{}};
    if(c.fm) {
        const auto def=backend?backend:FmBackendDefinition::builtin();const auto& d=def->api().descriptor;
        FmBackend chip(def,3579545);FmStateProfile f{};
        std::copy_n(d.backend_id,16,f.backendId.begin());std::copy_n(d.compatibility_id,16,f.compatibilityId.begin());
        f.stateVersion=d.state_version;f.clock=d.master_clock_hz;f.divider=d.native_divider;f.backendBytes=chip.stateSize();f.method=c.method;f.resamplerVersion=1;p.fm=f;
    }
    return p;
}
std::unique_ptr<EmulatorSystem> make(const RomImage& rom,const Case& c,const FmBackendPtr& backend) {
    return std::make_unique<EmulatorSystem>(rom,c.video,c.rate,c.machine,c.fm,c.method,backend);
}
void put(std::vector<uint8_t>& out,uint64_t value,unsigned width=8) {for(unsigned n=0;n<width;++n) {out.push_back(static_cast<uint8_t>(value));value>>=8;}}
uint64_t get(const std::vector<uint8_t>& bytes,size_t at,unsigned width) {uint64_t v=0;for(unsigned n=0;n<width;++n) v|=uint64_t(bytes[at+n])<<(8*n);return v;}
void set(std::vector<uint8_t>& bytes,size_t at,uint64_t value,unsigned width) {for(unsigned n=0;n<width;++n) {bytes[at+n]=static_cast<uint8_t>(value);value>>=8;}}
// Independent table CRC; the production codec uses a bitwise CRC.
void crc(std::vector<uint8_t>& bytes) {
    std::array<uint32_t,256> table{};
    for(uint32_t n=0;n<256;++n) {auto x=n;for(unsigned b=0;b<8;++b) x=(x>>1)^((x&1)?0xedb88320u:0);table[n]=x;}
    set(bytes,14,0,4);uint32_t value=~0u;
    for(auto b:bytes) value=table[(value^b)&255]^(value>>8);
    set(bytes,14,static_cast<uint32_t>(~value),4);
}
void pixels(std::vector<uint8_t>& out,const EmulatorSystem& system) {for(auto p:system.framePixels()) {out.push_back(p.r);out.push_back(p.g);out.push_back(p.b);out.push_back(p.a);}}
bool sameValues(const EmulatorSystemState& a,const EmulatorSystemState& b,const StateProfile& profile) {
    const auto& x=a.cpu;const auto& y=b.cpu;const auto& v=a.vdp;const auto& w=b.vdp;const auto& p=a.audio.psg;const auto& q=b.audio.psg;
    const auto& i=a.controller.pad;const auto& j=b.controller.pad;
    if(a.tstates!=b.tstates || !test::sameRegisters(x.registers,y.registers) || x.iff1!=y.iff1 || x.iff2!=y.iff2 || x.halted!=y.halted || x.flagsModified!=y.flagsModified || x.ldAir!=y.ldAir || x.eiDelay!=y.eiDelay || x.interruptMode!=y.interruptMode || x.memptr!=y.memptr ||
       a.memory.ram!=b.memory.ram || a.memory.ramEnabled!=b.memory.ramEnabled || a.memory.cartridgeEnabled!=b.memory.cartridgeEnabled || a.memoryControl.ioDisabled!=b.memoryControl.ioDisabled || a.cartridge.ram!=b.cartridge.ram ||
       i.up!=j.up || i.down!=j.down || i.left!=j.left || i.right!=j.right || i.button1!=j.button1 || i.button2!=j.button2 || i.start!=j.start || a.controller.control!=b.controller.control ||
       v.vram!=w.vram || v.address!=w.address || v.code!=w.code || v.buffer!=w.buffer || v.secondByte!=w.secondByte || v.line!=w.line || v.cycle!=w.cycle || v.frames!=w.frames || v.hLatch!=w.hLatch || v.registers!=w.registers || v.framePending!=w.framePending || v.linePending!=w.linePending || v.lineCounter!=w.lineCounter || v.cramLatch!=w.cramLatch || v.drawing!=w.drawing || v.scrollY!=w.scrollY || v.spriteStatus!=w.spriteStatus ||
       p.tones!=q.tones || p.volumes!=q.volumes || p.counters!=q.counters || p.outputs!=q.outputs || p.selected!=q.selected || p.noise!=q.noise || p.stereo!=q.stereo || p.divider!=q.divider || p.noiseCounter!=q.noiseCounter || p.lfsr!=q.lfsr || p.noisePhase!=q.noisePhase || p.noiseOutput!=q.noiseOutput ||
       a.audio.clock.phase!=b.audio.clock.phase || a.audio.time!=b.audio.time || a.audio.interval!=b.audio.interval || a.audio.area.left!=b.audio.area.left || a.audio.area.right!=b.audio.area.right || a.audio.fmControl!=b.audio.fmControl) return false;
    auto color=[](Rgba8 c,Rgba8 d){return c.r==d.r && c.g==d.g && c.b==d.b && c.a==d.a;};
    if(!std::equal(v.palette.begin(),v.palette.end(),w.palette.begin(),color)) return false;
    if (v.cramChangeCount != w.cramChangeCount) return false;
    for (unsigned i = 0; i < v.cramChangeCount; ++i) {
        const auto& a = v.cramChanges[i];
        const auto& b = w.cramChanges[i];
        if (a.dot != b.dot || a.index != b.index || !color(a.previous, b.previous)) return false;
    }
    for(unsigned n=0;n<2;++n) if(!std::equal(v.images[n].begin(),v.images[n].end(),w.images[n].begin(),color)) return false;
    if(profile.mapper==MapperKind::Sega) {const auto& m=std::get<SegaMapperState>(a.cartridge.mapper);const auto& n=std::get<SegaMapperState>(b.cartridge.mapper);if(m.banks!=n.banks || m.control!=n.control) return false;}
    if(profile.mapper==MapperKind::Codemasters) {const auto& m=std::get<CodemastersMapperState>(a.cartridge.mapper);const auto& n=std::get<CodemastersMapperState>(b.cartridge.mapper);if(m.banks!=n.banks || m.ramEnabled!=n.ramEnabled) return false;}
    if(profile.fm) {
        if(a.audio.fm->backend!=b.audio.fm->backend) return false;
        std::vector<uint8_t> first(FmResampler::stateBytes(profile.fm->method)),second(first.size());
        FmResampler::encodeState(a.audio.fm->resampler,profile.fm->method,first);FmResampler::encodeState(b.audio.fm->resampler,profile.fm->method,second);if(first!=second) return false;
    }
    return true;
}
std::vector<uint8_t> trace(EmulatorSystem& system) {
    std::vector<uint8_t> out;
    pixels(out,system);
    for(unsigned address=0;address<65536;++address) out.push_back(system.readMemory(static_cast<uint16_t>(address)));
    for(unsigned n=0;n<20000;++n) {
        const auto previous=system.completedFrames();
        put(out,std::get<uint32_t>(system.step({.right=n%7==0,.button1=n%31<12,.start=n%3==0},n==31 || n==300)),4);
        put(out,system.tstates());put(out,system.completedFrames());
        const auto cpu=system.cpu().captureState();const auto& r=cpu.registers;
        for(auto v:{r.af,r.bc,r.de,r.hl,r.afAlternate,r.bcAlternate,r.deAlternate,r.hlAlternate,r.ix,r.iy,r.sp,r.pc}) put(out,v,2);
        for(auto v:{r.i,r.r}) put(out,v,1);
        for(bool v:{cpu.iff1,cpu.iff2,cpu.halted,cpu.flagsModified,cpu.ldAir}) put(out,v,1);
        put(out,cpu.eiDelay,1);put(out,cpu.interruptMode,1);put(out,cpu.memptr,2);
        put(out,system.audioFrames().size(),4);
        for(auto frame:system.audioFrames()) {put(out,std::bit_cast<uint32_t>(frame.left),4);put(out,std::bit_cast<uint32_t>(frame.right),4);}
        if(system.completedFrames()!=previous) pixels(out,system);
    }
    for(unsigned address=0;address<65536;++address) out.push_back(system.readMemory(static_cast<uint16_t>(address)));
    return out;
}
std::vector<Case> cases(bool plugin) {
    std::vector<Case> result;
    if(!plugin) for(auto mapper:{MapperKind::RomOnly,MapperKind::Sega,MapperKind::Codemasters}) for(unsigned p=0;p<3;++p)
        result.push_back({p==2?Machine::GameGear:Machine::Sms,p==1?VideoStandard::Pal:VideoStandard::Ntsc,mapper,48000,false,FmInterpolation::Sinc});
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear}) for(uint32_t rate:{44100u,48000u,96000u})
        result.push_back({Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,rate,true,method});
    return result;
}
bool format() {
    const Case c{Machine::Sms,VideoStandard::Ntsc,MapperKind::RomOnly,48000,false,FmInterpolation::Sinc};const auto rom=image(c.mapper);const auto p=profile(rom,c,{});
    auto system=make(rom,c,{});auto state=std::make_unique<EmulatorSystemState>();system->captureState(*state);
    const auto bytes=encodeState(p,*state);auto independent=bytes;crc(independent);
    if(!(stateFileBytes(p)==418279 && bytes.size()==418279 && std::string_view(reinterpret_cast<const char*>(bytes.data()),8)=="ACESMSST" &&
        get(bytes,8,2)==2 && get(bytes,10,4)==418237 && get(bytes,18,4)==8192 && get(bytes,22,4)==rom.metadata().crc32 &&
        get(bytes,26,4)==0 && get(bytes,30,4)==0 && get(bytes,34,4)==48000 && get(bytes,38,4)==0 &&
        get(bytes,70,2)==0xdff8 && bytes[8277]==1 && bytes[8278]==1 && bytes[8287]==0xff && independent==bytes)) return false;
    auto withScroll = bytes;
    withScroll[withScroll.size()-2]=0x12;withScroll.back()=0x34;
    set(withScroll,8,1,2);
    crc(withScroll);
    auto scrollState = decodeState(withScroll, p);
    const auto* scroll = std::get_if<std::unique_ptr<EmulatorSystemState>>(&scrollState);
    auto updatedScroll = withScroll;
    set(updatedScroll,8,2,2);crc(updatedScroll);
    if (!std::holds_alternative<StateProfile>(inspectStateProfile(withScroll)) ||
        !scroll || encodeState(p, **scroll) != updatedScroll) return false;
    state->controller.player2={.down=true,.left=true,.button2=true};
    state->vdp.registers[8]=state->vdp.scrollX=state->vdp.nextScrollX=0x35;
    auto withPlayer2=encodeState(p,*state);
    if(withPlayer2[8288]!=0x26 || withPlayer2.size()!=bytes.size()) return false;
    auto decoded=decodeState(withPlayer2,p);
    const auto* current=std::get_if<std::unique_ptr<EmulatorSystemState>>(&decoded);
    if(!current || (*current)->vdp.scrollX!=0x35 || (*current)->vdp.nextScrollX!=0x35 ||
        encodeState(p,**current)!=withPlayer2) return false;
    auto legacy=withPlayer2;
    legacy.resize(legacy.size()-2);
    set(legacy,8,1,2);set(legacy,10,legacy.size()-42,4);crc(legacy);
    auto legacyState=decodeState(legacy,p);
    const auto* old=std::get_if<std::unique_ptr<EmulatorSystemState>>(&legacyState);
    if(!std::holds_alternative<StateProfile>(inspectStateProfile(legacy)) ||
        !old || encodeState(p,**old)!=withPlayer2) return false;
    set(legacy,8,2,2);crc(legacy);
    if(!std::holds_alternative<StateCodecError>(inspectStateProfile(legacy)) ||
        !std::holds_alternative<StateCodecError>(decodeState(legacy,p))) return false;
    auto previous=withPlayer2;
    set(previous,8,3,2);set(previous,10,previous.size()-42,4);crc(previous);
    decoded=decodeState(previous,p);
    if(!std::holds_alternative<StateCodecError>(inspectStateProfile(previous)) || !std::holds_alternative<StateCodecError>(decoded))
        return false;
    withPlayer2[8288]|=0x80;crc(withPlayer2);
    decoded=decodeState(withPlayer2,p);
    if(!std::holds_alternative<StateCodecError>(decoded) || std::get<StateCodecError>(decoded)!=StateCodecError::Value) return false;
    auto oldSms = bytes;
    oldSms.resize(418128);
    set(oldSms, 8, 1, 2);
    set(oldSms, 10, 418086, 4);
    crc(oldSms);
    const auto oldProfile = inspectStateProfile(oldSms);
    const auto* oldError = std::get_if<StateCodecError>(&oldProfile);
    if (!oldError || *oldError != StateCodecError::Size) return false;
    uint8_t id=0;
    for(auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear}) {
        auto selected=c;selected.fm=true;selected.method=method;const auto expected=profile(rom,selected,{});auto fmSystem=make(rom,selected,{});fmSystem->captureState(*state);
        const auto file=encodeState(expected,*state);if(file[file.size()-2-get(file,38,4)+56]!=id++) return false;
    }
    return true;
}
bool savedRate() {
    const Case c{Machine::Sms, VideoStandard::Ntsc, MapperKind::RomOnly, 44100, false, FmInterpolation::Sinc};
    const auto rom = image(c.mapper);
    const auto savedProfile = profile(rom, c, {});
    auto currentProfile = savedProfile;
    currentProfile.sampleRate = 48000;
    const auto system = make(rom, c, {});
    auto state = std::make_unique<EmulatorSystemState>();
    system->captureState(*state);
    const auto original = encodeState(savedProfile, *state);
    const auto decoded = decodeStateAtSavedRate(original, currentProfile);
    const auto* value = std::get_if<DecodedState>(&decoded);
    if (!value || value->profile.sampleRate != savedProfile.sampleRate ||
        encodeState(value->profile, *value->state) != original) return false;
    const auto ordinary = decodeState(original, currentProfile);
    const auto* ordinaryError = std::get_if<StateCodecError>(&ordinary);
    if (!ordinaryError || *ordinaryError != StateCodecError::Profile) return false;

    using Error = StateCodecError;
    auto rejects = [&](const auto& bytes, Error expected) {
        const auto result = decodeStateAtSavedRate(bytes, currentProfile);
        const auto* error = std::get_if<Error>(&result);
        return error && *error == expected;
    };
    auto invalid = original;
    set(invalid, 34, 0, 4);
    if (!rejects(invalid, Error::Checksum) || !rejects(std::span(invalid).first(invalid.size() - 1), Error::Size))
        return false;
    crc(invalid);
    if (!rejects(invalid, Error::Value)) return false;
    auto badFormat = invalid;
    badFormat[0] ^= 1;
    crc(badFormat);
    if (!rejects(badFormat, Error::Format)) return false;
    invalid[22] ^= 1;
    crc(invalid);
    return rejects(invalid, Error::Identity);
}

bool roundtrip(const FmBackendPtr& backend) {
    for(const auto c:cases(bool(backend))) {
        const auto rom=image(c.mapper);const auto p=profile(rom,c,backend);auto source=make(rom,c,backend),candidate=make(rom,c,backend);
        while(source->tstates()<80003) source->step();
        auto state=std::make_unique<EmulatorSystemState>();source->captureState(*state);const auto bytes=encodeState(p,*state);
        auto result=decodeState(bytes,p);if(!std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(result)) return false;
        const auto& restored=*std::get<std::unique_ptr<EmulatorSystemState>>(result);
        if(!sameValues(*state,restored,p) || encodeState(p,restored)!=bytes || candidate->restoreState(restored)!=ACESMS_FM_OK || trace(*source)!=trace(*candidate)) return false;
    }
    return true;
}
bool cramHistory() {
    const Case c{Machine::Sms, VideoStandard::Ntsc, MapperKind::RomOnly, 48000, false, FmInterpolation::Sinc};
    std::vector<uint8_t> code{0x3e, 0, 0xd3, 0xbf, 0x3e, 0xc0, 0xd3, 0xbf, 0x3e, 0, 0x3e, 0x3f};
    code.insert(code.end(), 42, 0); // First data OUT ends one T-state into line 1.
    for (unsigned i = 0; i < 22; ++i) code.insert(code.end(), {0xd3, 0xbe});
    code.resize(8192);
    const auto rom = std::get<RomImage>(RomLoader::fromBytes(std::move(code), {c.mapper}));
    const auto p = profile(rom, c, {});
    auto source = make(rom, c, {});
    auto candidate = make(rom, c, {});
    auto state = std::make_unique<EmulatorSystemState>();
    std::vector<uint8_t> continued;
    for (const auto [time, count] : {std::pair{229u, 1u}, {405u, 17u}, {449u, 21u}}) {
        while (source->tstates() < time) source->step();
        source->captureState(*state);
        if (state->tstates != time || state->vdp.cramChangeCount != count) return false;
        const auto original = encodeState(p, *state);
        const auto decoded = decodeState(original, p);
        const auto* restored = std::get_if<std::unique_ptr<EmulatorSystemState>>(&decoded);
        if (!restored || candidate->restoreState(**restored) != ACESMS_FM_OK) return false;
        while (candidate->tstates() < 460) candidate->step();
        candidate->captureState(*state);
        if (state->tstates != 460 || state->vdp.cramChangeCount != 1) return false;
        const auto replayed = encodeState(p, *state);
        if (continued.empty()) continued = replayed;
        else if (replayed != continued) return false;

        source->captureState(*state);
        if (count < maxCramChanges) {
            // A second change at the last dot cannot yet fit at either sampled time.
            state->vdp.cramChanges[count] = state->vdp.cramChanges[count - 1];
            ++state->vdp.cramChangeCount;
            const auto rejected = decodeState(encodeState(p, *state), p);
            const auto* error = std::get_if<StateCodecError>(&rejected);
            if (!error || *error != StateCodecError::Value) return false;
        }
    }
    source->step();
    source->captureState(*state);
    return encodeState(p, *state) == continued;
}

bool reject(const FmBackendPtr& backend) {
    const Case c{Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,48000,true,FmInterpolation::Sinc};const auto rom=image(c.mapper);const auto p=profile(rom,c,backend);auto source=make(rom,c,backend);
    while(source->tstates()<80003) source->step();auto state=std::make_unique<EmulatorSystemState>();source->captureState(*state);const auto original=encodeState(p,*state);
    auto error=[&](const auto& bytes,StateCodecError expected){const auto result=decodeState(bytes,p);return std::holds_alternative<StateCodecError>(result) && std::get<StateCodecError>(result)==expected;};
    for(size_t n=0;n<original.size();++n) if(!error(std::span(original).first(n),StateCodecError::Size)) return false;
    auto bad=original;bad.push_back(0);if(!error(bad,StateCodecError::Size)) return false;
    bad=original;bad.back()^=1;if(!error(bad,StateCodecError::Checksum)) return false;
    for(size_t offset:{0u,8u,26u,27u,28u,29u}) {bad=original;bad[offset]=255;crc(bad);if(!error(bad,StateCodecError::Format)) return false;}
    for(size_t offset:{18u,22u}) {bad=original;bad[offset]^=1;crc(bad);if(!error(bad,StateCodecError::Identity)) return false;}
    for(size_t offset:{26u,27u,28u,29u,30u,34u}) {bad=original;bad[offset]^=1;crc(bad);if(!error(bad,StateCodecError::Profile)) return false;}
    for(size_t offset:{10u,38u}) {bad=original;bad[offset]^=1;crc(bad);if(!error(bad,StateCodecError::Size)) return false;}
    for(size_t offset:{76u,77u,78u,79u,80u,8277u,8278u,8279u,16476u}) {bad=original;bad[offset]=2;crc(bad);if(!error(bad,StateCodecError::Value)) return false;}
    auto invalid=[&](auto mutate) {auto s=std::make_unique<EmulatorSystemState>(*state);mutate(*s);return error(encodeState(p,*s),StateCodecError::Value);};
    if(!invalid([](auto& s){s.cpu.interruptMode=3;}) || !invalid([](auto& s){s.cpu.eiDelay=2;}) || !invalid([](auto& s){s.vdp.address=0x4000;}) ||
       !invalid([](auto& s){s.vdp.code=4;}) || !invalid([](auto& s){++s.vdp.cycle;}) || !invalid([](auto& s){++s.vdp.frames;}) ||
       !invalid([](auto& s){s.vdp.drawing^=1;}) || !invalid([](auto& s){s.vdp.hLatch=0x94;}) || !invalid([](auto& s){s.vdp.spriteStatus=1;}) ||
       !invalid([](auto& s){s.vdp.palette[0].a=0;}) || !invalid([](auto& s){s.vdp.images[1][100].r=1;}) ||
       !invalid([](auto& s){s.audio.psg.tones[0]=1024;}) || !invalid([](auto& s){s.audio.psg.counters[1]=0;}) ||
       !invalid([](auto& s){s.audio.psg.volumes[2]=16;}) || !invalid([](auto& s){s.audio.psg.selected=8;}) ||
       !invalid([](auto& s){s.audio.psg.noiseCounter=129;}) || !invalid([](auto& s){s.audio.psg.noise=0;s.audio.psg.noiseCounter=17;}) || !invalid([](auto& s){s.audio.psg.lfsr=0;}) ||
       !invalid([](auto& s){++s.audio.psg.divider;}) || !invalid([](auto& s){++s.audio.time;}) ||
       !invalid([](auto& s){++s.audio.clock.phase;}) || !invalid([](auto& s){++s.audio.interval;}) ||
       !invalid([](auto& s){s.audio.area.left=std::numeric_limits<double>::infinity();}) || !invalid([](auto& s){s.audio.area.right=1e100;}) ||
       !invalid([](auto& s){s.audio.fmControl=4;})) return false;
    const auto fm=original.size()-2-get(original,38,4);
    const auto history = fm - 148;
    bad = original; bad[history] = 22; crc(bad);
    if (!error(bad, StateCodecError::Value)) return false;
    bad = original; bad[history] = 1; set(bad, history + 1, 257, 2); bad[history + 7] = 255; crc(bad);
    if (!error(bad, StateCodecError::Value)) return false;
    bad = original; bad[history] = 0; bad[history + 1] = 1; crc(bad);
    if (!error(bad, StateCodecError::Value)) return false;
    for(size_t offset:{0u,16u,20u,36u,40u,56u,57u}) {bad=original;bad[fm+offset]^=1;crc(bad);if(!error(bad,StateCodecError::Profile)) return false;}
    for(size_t offset:{44u,52u}) {bad=original;bad[fm+offset]^=1;crc(bad);if(!error(bad,StateCodecError::Value)) return false;}
    for(size_t offset:{59u,63u}) {bad=original;bad[fm+offset]^=1;crc(bad);if(!error(bad,StateCodecError::Size)) return false;}
    bad=original;bad[fm+67]^=1;crc(bad);auto opaque=decodeState(bad,p);
    if(!std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(opaque)) return false;
    auto candidate=make(rom,c,backend);
    if(candidate->restoreState(*std::get<std::unique_ptr<EmulatorSystemState>>(opaque))==ACESMS_FM_OK || candidate->tstates()!=0) return false;
    bad=original;bad[fm+67+p.fm->backendBytes]^=1;crc(bad);if(!error(bad,StateCodecError::Value)) return false;
    return true;
}
bool sg1000() {
    auto bytes = test::sg1000TestRom();
    bytes.resize(40960);
    auto rom = std::get<RomImage>(RomLoader::fromBytes(std::move(bytes),
        {.mapper = MapperKind::RomOnly, .machine = Machine::Sg1000}));
    const Case c{Machine::Sg1000, VideoStandard::Ntsc, MapperKind::RomOnly, 48000, false, FmInterpolation::Sinc};
    const auto p = profile(rom, c, {});
    auto source = make(rom, c, {}), candidate = make(rom, c, {});
    while (source->tstates() < 80003) source->step();
    source->step({},false,{.down=true,.left=true,.button2=true});
    auto state = std::make_unique<EmulatorSystemState>();
    source->captureState(*state);
    const auto blob = encodeState(p, *state);
    if (get(blob, 8, 2) != 2 || get(blob, 26, 1) != 2 || state->vdp.spriteStatus != 0x64 ||
        blob[8288]!=0x26 || get(blob,blob.size()-2,2)!=0) return false;
    const auto info = inspectStateProfile(blob);
    const auto* inspected = std::get_if<StateProfile>(&info);
    if (!inspected || inspected->machine != Machine::Sg1000 || inspected->romBytes != 40960 || inspected->fm) return false;
    auto restored = decodeState(blob, p);
    const auto* decoded = std::get_if<std::unique_ptr<EmulatorSystemState>>(&restored);
    if (!decoded || encodeState(p,**decoded)!=blob || !sameValues(*state, **decoded, p) || candidate->restoreState(**decoded) != ACESMS_FM_OK
        || trace(*source) != trace(*candidate)) return false;

    const auto error = [&](const auto& input, const StateProfile& expected, StateCodecError code) {
        const auto result = decodeState(input, expected);
        const auto* reason = std::get_if<StateCodecError>(&result);
        return reason && *reason == code;
    };
    auto other = p;
    other.machine = Machine::Sms;
    if (!error(blob, other, StateCodecError::Size)) return false;
    auto old=blob;
    old.resize(old.size()-2);
    set(old,10,old.size()-42,4);crc(old);
    if(!error(old,p,StateCodecError::Size) ||
        !std::holds_alternative<StateCodecError>(inspectStateProfile(old))) return false;
    old.erase(old.begin()+8288);
    set(old,8,1,2);set(old,10,old.size()-42,4);crc(old);
    if(!error(old,p,StateCodecError::Size) || !std::holds_alternative<StateCodecError>(inspectStateProfile(old))) return false;
    other.machine = Machine::GameGear;
    if (!error(old, other, StateCodecError::Profile) || !error(blob,other,StateCodecError::Size)) return false;
    other = p;
    ++other.romCrc32;
    if (!error(blob, other, StateCodecError::Identity)) return false;
    auto bad = blob;
    bad[27] = 1;
    crc(bad);
    other = p;
    other.video = VideoStandard::Pal;
    if (!error(bad, other, StateCodecError::Profile)) return false;
    bad=blob;bad.back()=1;crc(bad);
    if(!error(bad,p,StateCodecError::Value)) return false;
    state->audio.psg.lfsr |= 0x8000;
    return error(encodeState(p, *state), p, StateCodecError::Value);
}

bool sega768k() {
    std::vector<uint8_t> bytes(768 * 1024);
    for (size_t bank = 0; bank < 48; ++bank) bytes[bank * 0x4000 + 0x400] = static_cast<uint8_t>(bank);
    const auto loaded = RomLoader::fromBytes(std::move(bytes), {MapperKind::Sega});
    const auto* rom = std::get_if<RomImage>(&loaded);
    if (!rom) return false;
    const Case c{Machine::Sms, VideoStandard::Ntsc, MapperKind::Sega, 48000, false, FmInterpolation::Sinc};
    const auto p = profile(*rom, c, {});
    auto system = make(*rom, c, {});
    auto state = std::make_unique<EmulatorSystemState>();
    system->captureState(*state);
    const std::array<uint8_t, 3> banks{0x40, 0x2f, 0xff};
    std::get<SegaMapperState>(state->cartridge.mapper).banks = banks;
    const auto blob = encodeState(p, *state);
    const auto info = inspectStateProfile(blob);
    const auto* inspected = std::get_if<StateProfile>(&info);
    if (!inspected || inspected->romBytes != p.romBytes || inspected->romCrc32 != p.romCrc32 ||
        inspected->machine != p.machine || inspected->mapper != p.mapper) return false;
    const auto result = decodeState(blob, p);
    const auto* restored = std::get_if<std::unique_ptr<EmulatorSystemState>>(&result);
    if (!restored || std::get<SegaMapperState>((*restored)->cartridge.mapper).banks != banks ||
        system->restoreState(**restored) != ACESMS_FM_OK ||
        system->readMemory(0x0400) != 16 || system->readMemory(0x4400) != 47 || system->readMemory(0x8400) != 15) return false;
    auto wrongSize = p, wrongCrc = p;
    wrongSize.romBytes = 1024 * 1024;
    wrongCrc.romCrc32 ^= 1;
    for (const auto& wrong : {wrongSize, wrongCrc}) {
        const auto rejected = decodeState(blob, wrong);
        const auto* error = std::get_if<StateCodecError>(&rejected);
        if (!error || *error != StateCodecError::Identity) return false;
    }
    return true;
}

bool limits() {
    for(auto mapper:{MapperKind::RomOnly,MapperKind::Sega,MapperKind::Codemasters}) for(size_t ram:{0u,8192u,16384u,32768u}) {
        if((mapper==MapperKind::RomOnly && ram) || (mapper==MapperKind::Codemasters && ram>8192)) continue;
        const Case c{Machine::Sms,VideoStandard::Ntsc,mapper,48000,false,FmInterpolation::Sinc};const auto rom=image(mapper,ram);const auto p=profile(rom,c,{});auto system=make(rom,c,{});auto state=std::make_unique<EmulatorSystemState>();system->captureState(*state);
        state->cpu.registers=test::initialRegisters();state->cpu.iff1=state->cpu.iff2=state->cpu.flagsModified=state->cpu.ldAir=true;state->cpu.interruptMode=2;state->cpu.eiDelay=1;state->cpu.memptr=0xbeef;
        state->memory.ramEnabled=state->memory.cartridgeEnabled=false;state->memoryControl.ioDisabled=true;state->controller={{.up=true,.right=true,.button2=true,.start=true},0x55};
        for(size_t n=0;n<state->memory.ram.size();++n) state->memory.ram[n]=static_cast<uint8_t>(n*17);
        auto& v=state->vdp;v.address=0x3ffe;v.code=3;v.buffer=0xe2;v.secondByte=true;v.hLatch=0xe9;v.framePending=v.linePending=true;v.lineCounter=0x93;v.scrollY=0x9b;v.spriteStatus=0x60;
        for(size_t n=0;n<v.vram.size();++n) v.vram[n]=static_cast<uint8_t>(n*29);
        for(size_t n=0;n<v.registers.size();++n) v.registers[n]=static_cast<uint8_t>(0xf0+n);
        for(size_t n=0;n<v.palette.size();++n) v.palette[n]={static_cast<uint8_t>(n%4*85),85,170,255};
        for(unsigned n=0;n<2;++n) for(size_t k=0;k<v.images[n].size();++k) v.images[n][k]={static_cast<uint8_t>((k+n)%4*85),170,85,255};
        auto& g=state->audio.psg;g.tones={1023,0x123,0x2ab};g.counters={1022,0x122,0x2aa};g.volumes={1,2,3,4};g.outputs={true,false,true};g.selected=7;g.noise=7;g.stereo=0xa5;g.noiseCounter=128;g.lfsr=0xace1;g.noisePhase=g.noiseOutput=true;
        for(size_t n=0;n<ram;++n) state->cartridge.ram[n]=static_cast<uint8_t>(n*37);
        if(mapper==MapperKind::Sega) std::get<SegaMapperState>(state->cartridge.mapper)={{255,254,253},0xa5};
        if(mapper==MapperKind::Codemasters) std::get<CodemastersMapperState>(state->cartridge.mapper)={{255,127,254},true};
        auto result=decodeState(encodeState(p,*state),p);if(!std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(result)) return false;
        const auto& s=*std::get<std::unique_ptr<EmulatorSystemState>>(result);
        if(!sameValues(*state,s,p)) return false;
    }
    const Case c{Machine::Sms,VideoStandard::Ntsc,MapperKind::RomOnly,48000,false,FmInterpolation::Sinc};const auto rom=image(c.mapper);const auto p=profile(rom,c,{});auto system=make(rom,c,{});auto state=std::make_unique<EmulatorSystemState>();system->captureState(*state);
    for(unsigned extra:{0u,1u}) {
        const auto time=std::numeric_limits<uint64_t>::max()-EmulatorSystem::maximumStepTstates+extra;state->tstates=state->audio.time=state->vdp.cycle=time;
        state->vdp.frames=time/(262*228);state->vdp.line=static_cast<uint16_t>(time/228%262);state->vdp.drawing=static_cast<uint8_t>(1^(state->vdp.frames&1));
        state->audio.clock.phase=(time%3579545)*48000%3579545;state->audio.interval=state->audio.clock.phase/48000;state->audio.psg.divider=static_cast<uint8_t>(time%16);
        const auto result=decodeState(encodeState(p,*state),p);
        if(extra==0 && !std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(result)) return false;
        if(extra && (!std::holds_alternative<StateCodecError>(result) || std::get<StateCodecError>(result)!=StateCodecError::Value)) return false;
    }
    return true;
}
bool process(const std::filesystem::path& directory,const FmBackendPtr& backend,bool save) {
    std::filesystem::create_directories(directory);unsigned index=0;
    for(const auto c:cases(bool(backend))) {
        const auto rom=image(c.mapper);const auto p=profile(rom,c,backend);auto system=make(rom,c,backend);
        const auto stem=directory/std::to_string(index++);
        if(save) {
            while(system->tstates()<80003) system->step();auto s=std::make_unique<EmulatorSystemState>();system->captureState(*s);const auto bytes=encodeState(p,*s);const auto expected=trace(*system);
            std::ofstream state(stem.string()+".state",std::ios::binary),output(stem.string()+".trace",std::ios::binary);
            state.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));output.write(reinterpret_cast<const char*>(expected.data()),static_cast<std::streamsize>(expected.size()));if(!state || !output) return false;
        } else {
            std::ifstream state(stem.string()+".state",std::ios::binary),expected(stem.string()+".trace",std::ios::binary);if(!state || !expected) return false;
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(state)),{}),output((std::istreambuf_iterator<char>(expected)),{});
            auto result=decodeState(bytes,p);if(!std::holds_alternative<std::unique_ptr<EmulatorSystemState>>(result) || system->restoreState(*std::get<std::unique_ptr<EmulatorSystemState>>(result))!=ACESMS_FM_OK || !system->audioFrames().empty() || trace(*system)!=output) return false;
        }
    }
    return true;
}
}
int main(int argc,char** argv) {
    if(argc<3) return 1;FmBackendPtr backend;
#ifdef _WIN32
    if(argc==4) {const auto dll=std::filesystem::absolute(argv[3]);auto loaded=loadFmPlugin(dll.parent_path(),dll.filename());if(const auto* error=std::get_if<std::string>(&loaded)) {std::cerr<<*error;return 1;}backend=std::get<FmBackendPtr>(std::move(loaded));}
#endif
    const std::string_view op=argv[1];const bool passed=op=="format"?format() && savedRate():op=="roundtrip"?roundtrip(backend):op=="cram_history"?cramHistory():op=="reject"?reject(backend):op=="limits"?limits():op=="sg1000"?sg1000():op=="sega_768k"?sega768k():op=="save"?process(argv[2],backend,true):op=="load"&&process(argv[2],backend,false);
    if(!passed) std::cerr<<"State codec failed: "<<op<<'\n';return passed?0:1;
}
