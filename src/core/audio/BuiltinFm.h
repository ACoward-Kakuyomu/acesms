#pragma once

#include "acesms_fm.h"

namespace smsgg {
// Same v1 table contract as a DLL; no chip is constructed until create on the emulation worker.
const acesms_fm_api* builtinFmApi(uint32_t requestedAbi) noexcept;
} // namespace smsgg
