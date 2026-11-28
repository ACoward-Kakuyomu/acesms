#include "core/bus/MemoryControl.h"

namespace smsgg {
MemoryControl::MemoryControl(MemoryBus& memory, Z80Io& devices, Machine machine)
    : memory_(memory), devices_(devices), machine_(machine) {}

uint8_t MemoryControl::readPort(uint16_t port, uint64_t tstate) {
    if (ioDisabled_ && (port == 0xdc || port == 0xdd)) return 0xff;
    return devices_.readPort(port, tstate);
}
void MemoryControl::writePort(uint16_t port, uint8_t value, uint64_t tstate) {
    if (port == 0x3e) {
        ioDisabled_ = machine_ == Machine::Sms && (value & 4) != 0;
        memory_.setMemoryControl(machine_ == Machine::GameGear ? value & 0x10 : value);
    } else if (!(ioDisabled_ && port == 0x3f)) devices_.writePort(port, value, tstate);
}
} // namespace smsgg
