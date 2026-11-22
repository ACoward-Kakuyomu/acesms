#include "core/audio/Emu2413Native.h"

#include <bit>
#include <cassert>
#include <type_traits>

namespace smsgg {
namespace {
struct Encoder {
    std::span<uint8_t> bytes;
    size_t position{};
    template<class T> void operator()(T& value) {
        const auto bits=std::bit_cast<std::make_unsigned_t<T>>(value);
        for(size_t i=0;i<sizeof(T);++i) bytes[position++]=static_cast<uint8_t>(bits>>(i*8));
    }
    template<class T,size_t N> void operator()(std::array<T,N>& values) {for(auto& v:values) (*this)(v);}
};
struct Decoder {
    std::span<const uint8_t> bytes;
    size_t position{};
    template<class T> void operator()(T& value) {
        using U=std::make_unsigned_t<T>;
        U bits{};
        for(size_t i=0;i<sizeof(T);++i) bits|=static_cast<U>(bytes[position++])<<(i*8);
        value=std::bit_cast<T>(bits);
    }
    template<class T,size_t N> void operator()(std::array<T,N>& values) {for(auto& v:values) (*this)(v);}
};
// One explicit v1 field order shared by the byte encoder and decoder, no object representation.
void transfer(Emu2413NativeState& s,auto& io) {
    auto fields=[&](auto&... values) {(io(values),...);};
    fields(s.masterTicks,s.chipType,s.address,s.registers,s.testFlag,s.keyStatus,s.rhythmMode,
        s.egCounter,s.pmPhase,s.amPhase,s.lfoAm,s.noise,s.shortNoise,s.patchNumbers);
    for(auto& v:s.slots)
        fields(v.number,v.type,v.patch,v.wave,v.output,v.pgPhase,v.pgOut,v.pgKeep,v.blkFnum,v.fnum,v.blk,
            v.egState,v.volume,v.keyFlag,v.susFlag,v.tll,v.rks,v.egRateHigh,v.egRateLow,v.egShift,v.egOut,v.updateRequests);
    for(auto& p:s.patches) fields(p.tl,p.fb,p.eg,p.ml,p.ar,p.dr,p.sl,p.rr,p.kr,p.kl,p.am,p.pm,p.ws);
    fields(s.pan,s.panFine,s.mask,s.channelOutput,s.mixOutput);
}
bool valid(const Emu2413NativeState& s,uint64_t expectedMasterTicks) {
    if(s.masterTicks!=expectedMasterTicks || s.chipType!=0 || s.address>255 || s.keyStatus>=0x40000
        || s.rhythmMode>1 || s.rhythmMode!=((s.registers[0x0e]>>5)&1) || s.testFlag!=s.registers[0x0f]
        || s.egCounter!=static_cast<uint32_t>(s.masterTicks/72) || s.lfoAm>13 || !s.noise || s.noise>=0x800000
        || s.shortNoise>1 || s.mask>=0x4000) return false;
    for(auto n:s.patchNumbers) if(n<0 || n>18) return false;
    for(const auto& p:s.patches)
        if(p.tl>63 || p.fb>7 || p.eg>1 || p.ml>15 || p.ar>15 || p.dr>15 || p.sl>15 || p.rr>15
            || p.kr>1 || p.kl>3 || p.am>1 || p.pm>1 || p.ws>1) return false;
    for(size_t i=0;i<s.slots.size();++i) {
        const auto& v=s.slots[i];
        const auto type=s.rhythmMode && i>=14?3:i%2;
        if(v.number!=i || v.type!=type || v.patch>=38 || v.wave>=2 || v.pgPhase>=0x80000 || v.pgOut>=1024
            || v.pgKeep>1 || v.blkFnum>=4096 || v.fnum>=512 || v.blk>=8 || v.egState>=5
            || v.volume<0 || v.volume>60 || (v.volume&3) || v.keyFlag>1 || v.susFlag>1 || v.tll>238
            || v.rks>15 || v.egRateHigh>15 || v.egRateLow>3 || v.egShift>13 || v.egOut>127
            || (v.updateRequests>15 && v.updateRequests!=255)
            || v.pgOut!=(v.pgPhase>>9) || v.blkFnum!=((v.blk<<9)|v.fnum)) return false;
        for(auto y:v.output) if(y< -4096 || y>4095) return false;
    }
    for(auto p:s.pan) if(p>3) return false;
    for(const auto& pan:s.panFine) for(auto gain:pan) if(gain<0 || gain>4096) return false;
    return true;
}
} // namespace
void Emu2413Native::saveState(std::span<uint8_t> bytes) const noexcept {
    assert(bytes.size()>=stateBytes);
    auto state=captureState();Encoder writer{bytes};transfer(state,writer);
    assert(writer.position==stateBytes);
}
bool Emu2413Native::loadState(std::span<const uint8_t> bytes,uint64_t expectedMasterTicks) noexcept {
    if(bytes.size()!=stateBytes) return false;
    Emu2413NativeState state;
    Decoder reader{bytes};transfer(state,reader);
    assert(reader.position==stateBytes);
    if(!valid(state,expectedMasterTicks)) return false;
    restoreState(state);
    return true;
}
} // namespace smsgg
