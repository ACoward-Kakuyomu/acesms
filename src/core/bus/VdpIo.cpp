#include "core/bus/VdpIo.h"

namespace smsgg {
VdpIo::VdpIo(Vdp& vdp, Z80Io& otherDevices) : vdp_(vdp), otherDevices_(otherDevices) {}

uint8_t VdpIo::readPort(uint16_t port) {
    switch (port) {
    case 0xbe: return vdp_.readData();
    case 0xbf: return vdp_.readStatus();
    case 0x7e: return vdp_.readVCounter();
    case 0x7f: return vdp_.readHCounter();
    default: return otherDevices_.readPort(port);
    }
}

void VdpIo::writePort(uint16_t port, uint8_t value, uint64_t tstate) {
    if (port == 0xbe) vdp_.writeData(value, tstate);
    else if (port == 0xbf) vdp_.writeControl(value);
    else otherDevices_.writePort(port, value, tstate);
}
} // namespace smsgg
