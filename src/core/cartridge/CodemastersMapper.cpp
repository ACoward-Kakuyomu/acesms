#include "core/cartridge/CodemastersMapper.h"

namespace smsgg {
uint8_t CodemastersMapper::read8(uint16_t address) {
    if(address>=0xc000) return 0xff;
    if(ramEnabled_ && address>=0xa000) return ram_.empty()?0xff:ram_[address-0xa000];
    const size_t index=banks_[address/0x4000]*0x4000+(address&0x3fff);
    return rom_[index&(rom_.size()-1)];
}
void CodemastersMapper::write8(uint16_t address,uint8_t value) {
    if(address>=0xc000) return;
    // Provisional contract: an enabled RAM-window write never changes the bank latch.
    if(ramEnabled_ && address>=0xa000) {
        if(!ram_.empty()) ram_[address-0xa000]=value;
        return;
    }
    if(address>=0x4000 && address<0x8000) {
        ramEnabled_=(value&0x80)!=0;
        banks_[1]=value&0x7f;
    } else banks_[address/0x4000]=value;
}
void CodemastersMapper::reset() {banks_={0,1,0};ramEnabled_=false;}
} // namespace smsgg
