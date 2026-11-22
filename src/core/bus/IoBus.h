#pragma once

#include "core/cpu/Z80.h"
#include "core/Machine.h"

namespace smsgg {

// SMS/GG port decoding. Device behavior is handled by the downstream adapters.
class IoBus final : public Z80Io {
public:
    explicit IoBus(Z80Io& devices, Machine machine = Machine::Sms, bool fmEnabled = false);
    uint8_t readPort(uint16_t port) override;
    void writePort(uint16_t port, uint8_t value, uint64_t tstate = 0) override;
private:
    Z80Io& devices_;
    Machine machine_;
    bool fmEnabled_;
};

} // namespace smsgg
