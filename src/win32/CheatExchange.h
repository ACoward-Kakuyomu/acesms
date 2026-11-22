#pragma once

#include "win32/RomDatabase.h"

namespace smsgg::win32 {

struct CheatExchangeError {std::wstring message;};

std::variant<std::vector<Cheat>,CheatExchangeError> readCheatExchange(const std::filesystem::path& path,
    const RomIdentity& identity,Machine machine,std::span<const Cheat> draft);
std::optional<CheatExchangeError> writeCheatExchange(const std::filesystem::path& path,
    const RomIdentity& identity,Machine machine,std::span<const Cheat> saved);

} // namespace smsgg::win32
