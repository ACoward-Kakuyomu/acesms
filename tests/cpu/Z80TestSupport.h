#pragma once

#include "core/cpu/Z80.h"

namespace smsgg::test {

inline Z80Registers initialRegisters() {
    return {.af = 0x12ff, .bc = 0x2345, .de = 0x3456, .hl = 0x4567,
            .afAlternate = 0x5678, .bcAlternate = 0x6789,
            .deAlternate = 0x789a, .hlAlternate = 0x89ab,
            .ix = 0x9abc, .iy = 0xabcd, .sp = 0xbcde, .pc = 0x1234,
            .i = 0xcd, .r = 0x35};
}

inline bool sameRegisters(const Z80Registers& a, const Z80Registers& b) {
    return a.af == b.af && a.bc == b.bc && a.de == b.de && a.hl == b.hl
        && a.afAlternate == b.afAlternate && a.bcAlternate == b.bcAlternate
        && a.deAlternate == b.deAlternate && a.hlAlternate == b.hlAlternate
        && a.ix == b.ix && a.iy == b.iy && a.sp == b.sp && a.pc == b.pc
        && a.i == b.i && a.r == b.r;
}

} // namespace smsgg::test
