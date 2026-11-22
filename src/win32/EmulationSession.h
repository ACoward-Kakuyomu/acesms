#pragma once

#include "core/state/StateCodec.h"
#include "win32/SramStore.h"

namespace smsgg::win32 {
using SessionError=std::variant<FileError,StateCodecError,acesms_fm_result>;
class EmulationSession final {
public:
    EmulationSession(RomImage image,const std::filesystem::path& path,VideoStandard video,uint32_t rate,
        Machine machine=Machine::Sms,bool fm=false,FmInterpolation method=FmInterpolation::Sinc,FmBackendPtr backend={},std::unique_ptr<EmulatorSystem> prepared={});
    EmulatorSystem& system() noexcept {return *system_;}
    const StateProfile& profile() const noexcept {return profile_;}
    const RomMetadata& metadata() const noexcept {return rom_.metadata();}
    const RomImage& romImage() const noexcept {return rom_;}
    const FmBackendPtr& fmBackend() const noexcept {return backend_;}
    bool hasSram() const noexcept {return sram_.enabled();}
    const std::filesystem::path& sramPath() const noexcept {return sram_.path();}
    std::optional<SessionError> loadSram();
    std::optional<SessionError> saveSram();
    std::optional<SessionError> saveState(const std::filesystem::path& path) const;
    std::variant<std::unique_ptr<EmulatorSystem>,SessionError> prepareState(const std::filesystem::path& path) const;
    std::variant<std::unique_ptr<EmulatorSystem>,SessionError> prepareState(const EmulatorSystemState& decoded) const;
    // Worker commits a prepared candidate only after consumer stop acknowledgement.
    std::unique_ptr<EmulatorSystem> commitState(std::unique_ptr<EmulatorSystem> candidate) noexcept {
        system_.swap(candidate);
        return candidate;
    }
private:
    std::unique_ptr<EmulatorSystem> makeSystem() const;
    RomImage rom_;
    FmBackendPtr backend_;
    StateProfile profile_;
    SramStore sram_;
    std::unique_ptr<EmulatorSystem> system_;
};
}
