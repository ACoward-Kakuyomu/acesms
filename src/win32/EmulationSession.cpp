#include "win32/EmulationSession.h"

#include <algorithm>

namespace smsgg::win32 {
EmulationSession::EmulationSession(RomImage image,const std::filesystem::path& path,VideoStandard video,uint32_t rate,
    Machine machine,bool fm,FmInterpolation method,FmBackendPtr backend,std::unique_ptr<EmulatorSystem> prepared)
    : rom_(std::move(image)),backend_(fm?(backend?std::move(backend):FmBackendDefinition::builtin()):nullptr),sram_(path,rom_) {
    profile_={static_cast<uint32_t>(rom_.bytes().size()),rom_.metadata().crc32,machine,video,rom_.options().mapper,static_cast<uint32_t>(rom_.options().sramBytes),rate,{}};
    if(fm) {
        const auto& d=backend_->api().descriptor;FmStateProfile f{};
        std::copy_n(d.backend_id,16,f.backendId.begin());std::copy_n(d.compatibility_id,16,f.compatibilityId.begin());
        f.stateVersion=d.state_version;f.clock=d.master_clock_hz;f.divider=d.native_divider;f.method=method;f.resamplerVersion=1;profile_.fm=f;
    }
    system_=prepared?std::move(prepared):makeSystem();
    if(fm) {
        auto state=std::make_unique<EmulatorSystemState>();system_->captureState(*state);
        profile_.fm->backendBytes=static_cast<uint32_t>(state->audio.fm->backend.size());
    }
}
std::unique_ptr<EmulatorSystem> EmulationSession::makeSystem() const {
    return std::make_unique<EmulatorSystem>(rom_,profile_.video,profile_.sampleRate,profile_.machine,profile_.fm.has_value(),profile_.fm?profile_.fm->method:FmInterpolation::Sinc,backend_);
}
std::optional<SessionError> EmulationSession::loadSram() {
    if(!hasSram()) return std::nullopt;
    auto loaded=sram_.load();if(const auto* error=std::get_if<FileError>(&loaded)) return SessionError{*error};
    auto state=std::make_unique<EmulatorSystemState>();system_->captureState(*state);
    state->cartridge.ram=std::get<std::vector<uint8_t>>(std::move(loaded));
    const auto result=system_->restoreState(*state);if(result!=ACESMS_FM_OK) return SessionError{result};return std::nullopt;
}
std::optional<SessionError> EmulationSession::saveSram() {
    if(!hasSram()) return std::nullopt;
    auto state=std::make_unique<EmulatorSystemState>();system_->captureState(*state);
    if(auto error=sram_.save(state->cartridge.ram)) return SessionError{*error};return std::nullopt;
}
std::optional<SessionError> EmulationSession::saveState(const std::filesystem::path& path) const {
    auto state=std::make_unique<EmulatorSystemState>();system_->captureState(*state);
    if(auto error=writeFileAtomically(path,encodeState(profile_,*state))) return SessionError{*error};return std::nullopt;
}
std::variant<std::unique_ptr<EmulatorSystem>,SessionError> EmulationSession::prepareState(const std::filesystem::path& path) const {
    auto bytes=readFileExact(path,stateFileBytes(profile_));if(const auto* error=std::get_if<FileError>(&bytes)) return SessionError{*error};
    auto decoded=decodeState(std::get<std::vector<uint8_t>>(bytes),profile_);if(const auto* error=std::get_if<StateCodecError>(&decoded)) return SessionError{*error};
    return prepareState(*std::get<std::unique_ptr<EmulatorSystemState>>(decoded));
}
std::variant<std::unique_ptr<EmulatorSystem>,SessionError> EmulationSession::prepareState(const EmulatorSystemState& decoded) const {
    auto candidate=makeSystem();
    const auto result=candidate->restoreState(decoded);
    if(result!=ACESMS_FM_OK) return SessionError{result};
    return candidate;
}
}
