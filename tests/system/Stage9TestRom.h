#pragma once

#include "Stage6TestRom.h"
#include "Stage7TestRom.h"

namespace smsgg::test {
inline std::vector<uint8_t> stage9CodemastersRom(bool gg,bool withRam=false) {
    const auto base=gg?stage7GameGearRom():stage6AudioRom();
    const uint16_t entry=static_cast<uint16_t>(base[1]|(base[2]<<8));
    const uint16_t next=withRam?0x3200:entry;
    std::vector<uint8_t> rom(65536);
    std::copy(base.begin(),base.end(),rom.begin());
    std::copy(base.begin(),base.end(),rom.begin()+3*16384);
    rom[65535]=0xa3;
    std::vector<uint8_t> ram{
        0x3e,3,0x32,0,0,0x3a,0xff,0x3f,0x32,0x10,0xc0,
        0x3e,2,0x32,0,0x80,0xc3,0,0x81,
        0xaf,0x32,0,0,0x3a,0x10,0xc0,0xfe,0xa3,0xc2,0,0,
        0x3a,0x11,0xc0,0xfe,0xa2,0xc2,0,0,0xc3,
        static_cast<uint8_t>(next),static_cast<uint8_t>(next>>8),0x76};
    const auto fail=static_cast<uint16_t>(0xc100+ram.size()-1);
    for(const size_t offset:{29,37}) {ram[offset]=static_cast<uint8_t>(fail);ram[offset+1]=static_cast<uint8_t>(fail>>8);}
    const uint8_t bank2[]{0x3e,0xa2,0x32,0x11,0xc0,0xc3,0x13,0xc1};
    std::copy(std::begin(bank2),std::end(bank2),rom.begin()+0x8100);
    const uint8_t init[]{0xf3,0x31,0xf8,0xdf,0x21,0x12,0x30,0x11,0,0xc1,0x01,
        static_cast<uint8_t>(ram.size()),0,0xed,0xb0,0xc3,0,0xc1};
    std::copy(std::begin(init),std::end(init),rom.begin()+0x3000);
    std::copy(ram.begin(),ram.end(),rom.begin()+0x3012);
    rom[0]=0xc3;rom[1]=0;rom[2]=0x30;
    if(withRam) {
        rom[0x9fff]=0xa2;rom[0xdfff]=0xa3;rom[0xe000]=0x93;
        size_t at=0x3200;
        auto emit=[&](std::initializer_list<uint8_t> code){for(auto byte:code) rom[at++]=byte;};
        std::vector<size_t> failures;
        auto check=[&](uint16_t address,uint8_t value) {
            emit({0x3a,static_cast<uint8_t>(address),static_cast<uint8_t>(address>>8),0xfe,value,0xc2,0,0});
            failures.push_back(at-2);
        };
        emit({0x3e,0x81,0x32,0,0x40,0x21,0,0xa0,0x11,1,0xa0,0x01,0xff,0x1f,0x36,0x5a,0xed,0xb0});
        emit({0x21,0,0xa0,0x01,0,0x20});
        const auto loop=static_cast<uint16_t>(at);
        emit({0x7e,0xfe,0x5a,0xc2,0,0});failures.push_back(at-2);
        emit({0x23,0x0b,0x78,0xb1,0xc2,static_cast<uint8_t>(loop),static_cast<uint8_t>(loop>>8)});
        emit({0x3e,0xaa,0x32,0xff,0xbf});
        check(0xa000,0x5a);check(0xbfff,0xaa);check(0x9fff,0xa2);
        emit({0x3e,3,0x32,0,0x80});check(0x9fff,0xa3);check(0xbfff,0xaa);
        emit({0x3e,1,0x32,0,0x40});check(0xa000,0x93);check(0xbfff,0xa3);
        emit({0x3e,0x81,0x32,0,0x40});check(0xa000,0x5a);check(0xbfff,0xaa);
        emit({0x3a,0,0xa0,0x32,0x12,0xc0,0x3a,0xff,0xbf,0x32,0x13,0xc0,
            0xaf,0x32,0,0x80,0xc3,static_cast<uint8_t>(entry),static_cast<uint8_t>(entry>>8)});
        const auto failAddress=static_cast<uint16_t>(at);emit({0x76});
        for(auto operand:failures) {rom[operand]=static_cast<uint8_t>(failAddress);rom[operand+1]=static_cast<uint8_t>(failAddress>>8);}
    }
    std::copy_n("TMR SEGA",8,rom.begin()+0x7ff0);rom[0x7fff]=gg?0x6e:0x4e;
    rom[0x7fe0]=4;rom[0x7fe1]=1;rom[0x7fe2]=1;rom[0x7fe3]=0x94;
    // Independent byte-lane sums, rather than the product's word-at-a-time loop.
    uint32_t low=0,high=0;
    for(size_t at=0;at<rom.size();++at) {
        if(at>=0x7ff0 && at<0x8000) continue;
        if(at&1) high+=rom[at];else low+=rom[at];
    }
    const auto sum=static_cast<uint16_t>(low+256*high),negative=static_cast<uint16_t>(0u-sum);
    rom[0x7fe6]=static_cast<uint8_t>(sum);rom[0x7fe7]=static_cast<uint8_t>(sum>>8);
    rom[0x7fe8]=static_cast<uint8_t>(negative);rom[0x7fe9]=static_cast<uint8_t>(negative>>8);
    return rom;
}
} // namespace smsgg::test
