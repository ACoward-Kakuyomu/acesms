#pragma once

#include <cstddef>

namespace smsgg {
enum class Machine { Sms = 0, GameGear = 1, Sg1000 = 2 };
constexpr bool hasSecondPad(Machine machine) { return machine!=Machine::GameGear; }
constexpr size_t workRamBytes(Machine machine) { return machine == Machine::Sg1000 ? 0x400 : 0x2000; }
}
