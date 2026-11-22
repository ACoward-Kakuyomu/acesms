#include "win32/MainWindow.h"
#include "win32/AudioOutput.h"
#include "win32/DatabasePath.h"

#include <memory>
#include <system_error>

namespace {
struct ComApartment {
    ComApartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result)) throw std::system_error(result, std::system_category(), "CoInitializeEx");
    }
    ~ComApartment() { CoUninitialize(); }
};
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    try {
        ComApartment com;
        smsgg::win32::WtlApplication application(instance);
        smsgg::win32::AudioOutput audio;
        auto portablePath = smsgg::win32::resolveDatabasePath({});
        if (const auto* error = std::get_if<std::string>(&portablePath)) throw std::runtime_error(*error);
        auto databasePath = std::get<std::filesystem::path>(std::move(portablePath));
        auto window = std::make_unique<smsgg::win32::MainWindow>(instance,audio.stream(),std::move(databasePath));
        return window->run(showCommand);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "SMS/GG startup, audio or display error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
