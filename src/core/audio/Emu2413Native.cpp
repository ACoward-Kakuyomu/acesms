#include "core/audio/Emu2413Native.h"

#include <emu2413.h>

#include <algorithm>
#include <cassert>
#include <new>

namespace smsgg {
Emu2413Native::Emu2413Native() : chip_(OPLL_new(72,1)) {
    if(!chip_) throw std::bad_alloc();
    waves_[0]=chip_->slot[0].wave_table;
    OPLL_writeIO(chip_,0,3);OPLL_writeIO(chip_,1,0x18);
    OPLL_calc(chip_); // Bootstrap the second legal table via public APIs, before emulated time starts.
    waves_[1]=chip_->slot[0].wave_table;
    reset();
}
Emu2413Native::~Emu2413Native() { OPLL_delete(chip_); }
void Emu2413Native::reset() {
    OPLL_reset(chip_);
    OPLL_resetPatch(chip_,OPLL_2413_TONE);
    time_=0;
}
size_t Emu2413Native::advanceTo(uint64_t target,std::span<int32_t> output) {
    assert(target>=time_);
    const auto count=target/72-time_/72;
    assert(count<=output.size());
    for(size_t i=0;i<count;++i) output[i]=OPLL_calc(chip_);
    time_=target;
    return static_cast<size_t>(count);
}
void Emu2413Native::writeIo(uint8_t addressOrData,uint8_t value) { OPLL_writeIO(chip_,addressOrData,value); }
Emu2413NativeState Emu2413Native::captureState() const noexcept {
    Emu2413NativeState s;
    s.masterTicks=time_;s.chipType=chip_->chip_type;s.address=chip_->adr;
    std::copy_n(chip_->reg,64,s.registers.begin());
    s.testFlag=chip_->test_flag;s.keyStatus=chip_->slot_key_status;s.rhythmMode=chip_->rhythm_mode;
    s.egCounter=chip_->eg_counter;s.pmPhase=chip_->pm_phase;s.amPhase=chip_->am_phase;
    s.lfoAm=chip_->lfo_am;s.noise=chip_->noise;s.shortNoise=chip_->short_noise;
    std::copy_n(chip_->patch_number,9,s.patchNumbers.begin());
    for(size_t i=0;i<s.patches.size();++i) {
        const auto& p=chip_->patch[i];
        s.patches[i]={p.TL,p.FB,p.EG,p.ML,p.AR,p.DR,p.SL,p.RR,p.KR,p.KL,p.AM,p.PM,p.WS};
    }
    for(size_t i=0;i<s.slots.size();++i) {
        const auto& p=chip_->slot[i];auto& v=s.slots[i];
        v.number=p.number;v.type=p.type;v.patch=static_cast<uint8_t>(p.patch-chip_->patch);v.wave=p.wave_table==waves_[1]?1:0;
        std::copy_n(p.output,2,v.output.begin());
        v.pgPhase=p.pg_phase;v.pgOut=p.pg_out;v.pgKeep=p.pg_keep;v.blkFnum=p.blk_fnum;v.fnum=p.fnum;v.blk=p.blk;
        v.egState=p.eg_state;v.volume=p.volume;v.keyFlag=p.key_flag;v.susFlag=p.sus_flag;
        v.tll=p.tll;v.rks=p.rks;v.egRateHigh=p.eg_rate_h;v.egRateLow=p.eg_rate_l;
        v.egShift=p.eg_shift;v.egOut=p.eg_out;v.updateRequests=p.update_requests;
    }
    std::copy_n(chip_->pan,16,s.pan.begin());
    for(size_t i=0;i<s.panFine.size();++i) std::copy_n(chip_->pan_fine[i],2,s.panFine[i].begin());
    s.mask=chip_->mask;
    std::copy_n(chip_->ch_out,14,s.channelOutput.begin());std::copy_n(chip_->mix_out,2,s.mixOutput.begin());
    return s;
}
void Emu2413Native::restoreState(const Emu2413NativeState& s) noexcept {
    time_=s.masterTicks;chip_->chip_type=s.chipType;chip_->adr=s.address;
    std::copy(s.registers.begin(),s.registers.end(),chip_->reg);
    chip_->test_flag=s.testFlag;chip_->slot_key_status=s.keyStatus;chip_->rhythm_mode=s.rhythmMode;
    chip_->eg_counter=s.egCounter;chip_->pm_phase=s.pmPhase;chip_->am_phase=s.amPhase;
    chip_->lfo_am=s.lfoAm;chip_->noise=s.noise;chip_->short_noise=s.shortNoise;
    std::copy(s.patchNumbers.begin(),s.patchNumbers.end(),chip_->patch_number);
    for(size_t i=0;i<s.patches.size();++i) {
        const auto& p=s.patches[i];
        chip_->patch[i]={p.tl,p.fb,p.eg,p.ml,p.ar,p.dr,p.sl,p.rr,p.kr,p.kl,p.am,p.pm,p.ws};
    }
    for(size_t i=0;i<s.slots.size();++i) {
        const auto& v=s.slots[i];auto& p=chip_->slot[i];
        p.number=v.number;p.type=v.type;p.patch=&chip_->patch[v.patch];p.wave_table=waves_[v.wave];
        std::copy(v.output.begin(),v.output.end(),p.output);
        p.pg_phase=v.pgPhase;p.pg_out=v.pgOut;p.pg_keep=v.pgKeep;p.blk_fnum=v.blkFnum;p.fnum=v.fnum;p.blk=v.blk;
        p.eg_state=v.egState;p.volume=v.volume;p.key_flag=v.keyFlag;p.sus_flag=v.susFlag;
        p.tll=v.tll;p.rks=v.rks;p.eg_rate_h=v.egRateHigh;p.eg_rate_l=v.egRateLow;
        p.eg_shift=v.egShift;p.eg_out=v.egOut;p.update_requests=v.updateRequests;
    }
    std::copy(s.pan.begin(),s.pan.end(),chip_->pan);
    for(size_t i=0;i<s.panFine.size();++i) std::copy(s.panFine[i].begin(),s.panFine[i].end(),chip_->pan_fine[i]);
    chip_->mask=s.mask;
    std::copy(s.channelOutput.begin(),s.channelOutput.end(),chip_->ch_out);std::copy(s.mixOutput.begin(),s.mixOutput.end(),chip_->mix_out);
}
} // namespace smsgg
