#pragma once

#include "core/cpu/Z80.h"

#include <array>
#include <variant>

namespace smsgg {
struct SegaMapperState {
    std::array<uint8_t,3> banks{0,1,2};
    uint8_t control{};
};
struct CodemastersMapperState {
    std::array<uint8_t,3> banks{0,1,0};
    bool ramEnabled{};
};
using MapperState=std::variant<std::monostate,SegaMapperState,CodemastersMapperState>;

class IMapper : public Z80Memory {
public:
    virtual void reset() = 0;
    virtual MapperState captureState() const = 0;
    virtual void restoreState(const MapperState& state) = 0;
};
} // namespace smsgg
