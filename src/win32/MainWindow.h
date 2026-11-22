#pragma once

#include "win32/D3D11Presenter.h"
#include "win32/EmulatorRunner.h"
#include "win32/InputManager.h"
#include "win32/Resource.h"
#include "win32/WtlApplication.h"
#include "win32/XInputController.h"

#include <atlframe.h>

#include <chrono>
#include <exception>

namespace smsgg::win32 {
class CheatDialog;
class MainWindow final : public WTL::CFrameWindowImpl<MainWindow> {
public:
    DECLARE_FRAME_WND_CLASS_EX(L"SmsGgStage5Window", IDI_ACESMS, 0, -1)
    MainWindow(HINSTANCE instance, AudioStream& audio, std::filesystem::path databasePath);
    ~MainWindow() override;
    HWND handle() const { return m_hWnd; }
    void openRom(const std::filesystem::path& path);
    bool pumpOnce(DWORD timeout = INFINITE);
    int run(int showCommand);
    BEGIN_MSG_MAP(MainWindow)
        try {
            MESSAGE_HANDLER(WM_COMMAND, onCommandMessage)
            MESSAGE_HANDLER(WM_DROPFILES, onDropFiles)
            MESSAGE_HANDLER(WM_CONTEXTMENU, onContextMenu)
            MESSAGE_HANDLER(WM_SETCURSOR, onSetCursor)
            MESSAGE_HANDLER(WM_KEYDOWN, onKey)
            MESSAGE_HANDLER(WM_KEYUP, onKey)
            MESSAGE_HANDLER(WM_SYSKEYDOWN, onKey)
            MESSAGE_HANDLER(WM_SYSKEYUP, onKey)
            MESSAGE_HANDLER(WM_SYSCHAR, onKey)
            MESSAGE_HANDLER(WM_SETFOCUS, onFocus)
            MESSAGE_HANDLER(WM_KILLFOCUS, onFocus)
            MESSAGE_HANDLER(WM_ACTIVATE, onFocus)
            MESSAGE_HANDLER(WM_TIMER, onInputTimer)
            MESSAGE_HANDLER(WM_SIZE, onSize)
            MESSAGE_HANDLER(WM_DPICHANGED, onDpiChanged)
            MESSAGE_HANDLER(WM_ERASEBKGND, onEraseBackground)
            MESSAGE_HANDLER(WM_PAINT, onPaint)
            MESSAGE_HANDLER(WM_CLOSE, onClose)
            MESSAGE_HANDLER(WM_DESTROY, onDestroy)
            CHAIN_MSG_MAP(WTL::CFrameWindowImpl<MainWindow>)
        } catch (const std::exception&) {
            failure_ = std::current_exception();
            runner_.stop();
            ::PostQuitMessage(1);
            lResult = uMsg == WM_NCCREATE ? FALSE : 0;
            return TRUE;
        }
    END_MSG_MAP()
private:
    friend struct MainWindowTestAccess;
    LRESULT onCommandMessage(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onContextMenu(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onSetCursor(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onDropFiles(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onKey(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onFocus(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onInputTimer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onSize(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onDpiChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onEraseBackground(UINT, WPARAM, LPARAM, BOOL&) { return 1; }
    LRESULT onPaint(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onClose(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT onDestroy(UINT, WPARAM, LPARAM, BOOL&);
    void onCommand(unsigned id);
    void chooseRom();
    void submitRom(LoadRequest request);
    void editRomSettings();
    void chooseState(bool save);
    void showStateManager();
    void openCheatDialog(bool editor);
    void updateStateSlots();
    unsigned activeStateSlot_{1};
    bool showStateManagerPending_{};
    std::vector<StateSummary> stateSlots_;
    std::optional<bool> pendingCheatEditor_;
    MemorySearch memorySearch_;
    std::unique_ptr<CheatDialog> cheatDialog_;
    void requestState(const std::filesystem::path& path,bool save);
    void operationStarted(RunnerOperation operation);
    bool canOperate() const;
    class SettingsDialog;
    void readSettings();
    bool saveSettings(const RomDefaults& next, const ControllerConfiguration& controllers, HWND owner);
    void showSettings();
    void showSettingsError(std::string_view reason, HWND owner);
    void updateMenu();
    void sendInput(bool consolePause = false);
    void consumeSnapshot();
    void updateFps(bool reset);
    void render(bool retryOccluded);
    void toggleFullscreen();
    void cycleWindowScale(const RECT& workArea);
    void refreshClientCursor();
    void close();
    void finishClose();
    std::unique_ptr<D3D11Presenter> presenter_;
    std::filesystem::path databasePath_;
    std::string startupSettingsError_;
    EmulatorRunner runner_;
    InputManager input_;
    XInputController gamepad_;
    static constexpr UINT_PTR inputTimerId = 1;
    RunnerSnapshot snapshot_;
    bool fpsVisible_{};
    std::optional<std::chrono::steady_clock::time_point> fpsSampleTime_;
    uint64_t fpsSampleFrames_{};
    RomDefaults romDefaults_;
    std::vector<std::filesystem::path> fmPlugins_;
    std::optional<LoadRequest> pendingRom_;
    std::optional<RomUpdateRequest> pendingEdit_;
    // An empty path selects the standard DB; legacy file-mode tests disengage the option.
    std::optional<std::filesystem::path> romDatabase_{std::filesystem::path{}};
    bool closeRequested_{};
    uint64_t handledSerial_{};
    std::wstring operationText_;
    bool paused_{}, muted_{}, fastForwardRequested_{}, minimized_{}, inputActive_{};
    bool settingsOpen_{};
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};
    WTL::CMenu fullscreenMenu_; // Owns the menu while it is detached from the HWND.
    bool fullscreenEnterDown_{};
    bool fullscreenPopupOpen_{};
    std::wstring title_;
    std::exception_ptr failure_;
};
} // namespace smsgg::win32
