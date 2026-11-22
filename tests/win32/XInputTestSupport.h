#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Xinput.h>

#include <array>
#include <vector>

namespace smsgg::test::xinput {
inline std::array<XINPUT_STATE, XUSER_MAX_COUNT> states{};
inline std::array<DWORD, XUSER_MAX_COUNT> results{};
inline std::vector<DWORD> calls;
inline void reset() {
    states = {};
    results.fill(ERROR_DEVICE_NOT_CONNECTED);
    calls.clear();
}
inline DWORD WINAPI read(DWORD index, XINPUT_STATE* state) noexcept {
    calls.push_back(index);
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    *state = states[index]; // Also write on failure, to catch use of invalid output.
    return results[index];
}
} // namespace smsgg::test::xinput
