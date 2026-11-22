#pragma once

#include "core/cpu/Z80.h"
#include "core/video/Vdp.h"

namespace smsgg {
class VdpIo final : public Z80Io {
public:
    VdpIo(Vdp& vdp, Z80Io& otherDevices);
    uint8_t readPort(uint16_t port) override;
    void writePort(uint16_t port, uint8_t value, uint64_t tstate = 0) override;
private:
    Vdp& vdp_;
    Z80Io& otherDevices_;
};
} // namespace smsgg
