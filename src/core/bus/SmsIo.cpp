#include "core/bus/SmsIo.h"

namespace smsgg {
uint8_t SmsIo::readPort(uint16_t port) {
    if(fmEnabled_ && port==0xf2) return audio_.readFmControl();
    if (machine_ == Machine::GameGear) {
        if (port == 0) return controller_.startPressed() ? 0x40 : 0xc0;
        if (port >= 1 && port <= 5) {
            constexpr uint8_t idle[]{0x7f,0xff,0,0xff,0};
            return idle[port-1];
        }
    }
    if (port == 0xdc) return controller_.readDc(machine_!=Machine::Sg1000);
    if (port == 0xdd) {
        if (machine_ == Machine::Sg1000) return controller_.readDd(false);
        return machine_ == Machine::GameGear ? controller_.readDd() | 0x8f : controller_.readDd();
    }
    // Remaining canonical reads are the unconnected SMS 2 3E/3F ports.
    return 0xff;
}

void SmsIo::writePort(uint16_t port, uint8_t value, uint64_t) {
    if(fmEnabled_ && (port==0xf0 || port==0xf1)) audio_.writeFm(static_cast<uint8_t>(port&1),value);
    else if(fmEnabled_ && port==0xf2) audio_.writeFmControl(value);
    else if (machine_ == Machine::GameGear && port == 6) audio_.writeStereo(value);
    else if (port == 0x3f) controller_.writeControl(value);
    else if (port == 0x7f) audio_.writePsg(value);
}
} // namespace smsgg
