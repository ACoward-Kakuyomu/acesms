#pragma once

#include "core/EmulatorSystem.h"

#include <array>
#include <memory>
#include <optional>

namespace smsgg {
struct FmStateProfile {
    std::array<uint8_t,16> backendId{};
    uint32_t stateVersion{};
    std::array<uint8_t,16> compatibilityId{};
    uint32_t clock{},divider{},backendBytes{};
    FmInterpolation method{};
    uint16_t resamplerVersion{};
    bool operator==(const FmStateProfile&) const=default;
};
// Ordinary loads use current validated configuration. Inspected profiles still require full decode/backend validation.
struct StateProfile {
    uint32_t romBytes{},romCrc32{};
    Machine machine{};
    VideoStandard video{};
    MapperKind mapper{};
    uint32_t ramBytes{},sampleRate{};
    std::optional<FmStateProfile> fm;
};
enum class StateCodecError {Format,Size,Checksum,Identity,Profile,Value};
size_t stateFileBytes(const StateProfile& profile, bool withVdpTrailer = true);
bool supportsStateFileSize(const StateProfile& profile, size_t bytes);
std::vector<uint8_t> encodeState(const StateProfile& profile,const EmulatorSystemState& state);
std::variant<std::unique_ptr<EmulatorSystemState>,StateCodecError> decodeState(std::span<const uint8_t> bytes,const StateProfile& profile);
struct DecodedState {StateProfile profile;std::unique_ptr<EmulatorSystemState> state;};
// RAM conversion only: retain the saved rate, while checking every other profile field.
std::variant<DecodedState,StateCodecError> decodeStateAtSavedRate(std::span<const uint8_t> bytes,StateProfile expected);
// Read bounded profile fields for resource resolution; decodeState must still validate CRC and values.
std::variant<StateProfile,StateCodecError> inspectStateProfile(std::span<const uint8_t> bytes);
}
