#include "core/bus/IoBus.h"
#include "IoTestSupport.h"

#include <iostream>

int main() {
    smsgg::test::Io devices;
    smsgg::IoBus io(devices, smsgg::Machine::Sms, true);
    for (unsigned port = 0; port < 65536; ++port) {
        const unsigned low = port & 255;
        const uint16_t read = low == 0xf2 ? 0xf2
            : static_cast<uint16_t>((low < 64 ? 0x3e : low < 128 ? 0x7e : low < 192 ? 0xbe : 0xdc) | (low & 1));
        const int write = low < 64 ? (0x3e | (low & 1)) : low < 128 ? 0x7f
            : low < 192 ? (0xbe | (low & 1)) : low >= 0xf0 && low <= 0xf2 ? low : -1;
        devices.reply = 0x5a; devices.accesses.clear();
        if (io.readPort(static_cast<uint16_t>(port)) != 0x5a || devices.accesses.size() != 1
            || devices.accesses[0].port != read || devices.accesses[0].write) return 1;
        devices.accesses.clear(); io.writePort(static_cast<uint16_t>(port), 0xa5);
        if (devices.accesses.size() != (write < 0 ? 0u : 1u)) return 1;
        if (write >= 0 && (devices.accesses[0].port != write || !devices.accesses[0].write
            || devices.accesses[0].value != 0xa5)) return 1;
    }
    return 0;
}
