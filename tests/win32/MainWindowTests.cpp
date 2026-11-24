#include "win32/MainWindow.h"
#include "win32/Resource.h"
#include "win32/Viewport.h"
#include "VersionResource.h"
#include "PresenterTestSupport.h"
#include "FakeAudioConsumer.h"
#include "ZipRomTestSupport.h"
#include "XInputTestSupport.h"
#include "../system/Stage5TestRom.h"
#include "../system/Sg1000TestRom.h"
#include "../system/Stage7TestRom.h"
#include "../system/Stage9TestRom.h"
#include "../cartridge/CodemastersTestSupport.h"
#include "win32/SramStore.h"
#include "win32/FmPlugin.h"
#include "win32/EmulationSession.h"
#include "win32/DbSramStore.h"

#include <shlobj.h>
#include <commctrl.h>
#include <sqlite3.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

namespace smsgg::win32 {
struct MainWindowTestAccess {
    static const RunnerSnapshot& snapshot(const MainWindow& w) { return w.snapshot_; }
    static void database(MainWindow& w,std::optional<std::filesystem::path> path) { w.romDatabase_=std::move(path); }
    static void useLegacyFixtureDefaults(MainWindow& w) {
        w.romDefaults_.options={MapperKind::Sega};
        w.romDefaults_.machine=Machine::Sms;
        w.romDefaults_.fmEnabled=false;
        w.updateMenu();
    }
    static Rgba8 firstDisplayed(MainWindow& w) {
        RECT client{};
        if (!GetClientRect(w.handle(), &client)) throw std::runtime_error("Get client size failed");
        const auto area=w.snapshot_.displayArea;
        const auto viewport = calculateViewport(client.right, client.bottom,area.width,area.height);
        const auto pixels = PresenterTestAccess::capture(*w.presenter_, client.right, client.bottom);
        return pixels[viewport.y * client.right + viewport.x];
    }
    static bool color(MainWindow& w, uint8_t r, uint8_t g) {
        const auto p=firstDisplayed(w);
        return p.r == r && p.g == g && p.b == 0 && p.a == 255;
    }
    static bool stopped(MainWindow& w) { return w.runner_.takeSnapshot().state == RunnerState::Stopped; }
    static void useTestGamepad(MainWindow& w) { w.gamepad_ = XInputController(test::xinput::read); }
    static void useGamepadInput(MainWindow& w,bool enabled=true) {
        auto settings=defaultControllerConfiguration();
        if(enabled) settings[0].source=PadSource::XInput;
        w.input_.configure(settings);
    }
    static void pollGamepad(MainWindow& w) { SendMessageW(w.handle(), WM_TIMER, MainWindow::inputTimerId, 0); }
    static PadState pad(MainWindow& w,unsigned player=0) { return w.input_.pad(player); }
    static unsigned rapid(MainWindow& w,unsigned player) {return w.input_.rapidButtons(player);}
    static const auto& fpsText(const MainWindow& w) { return PresenterTestAccess::fpsText(*w.presenter_); }
    static const auto& slotNotice(const MainWindow& w) { return PresenterTestAccess::slotNotice(*w.presenter_); }
    static void expireSlotNotice(MainWindow& w) { w.slotNoticeUntil_=std::chrono::steady_clock::now();pollGamepad(w); }
    static void cycleWindowScale(MainWindow& w,const RECT& workArea) { w.cycleWindowScale(workArea); }
    static void stateFile(MainWindow& w,const std::filesystem::path& path,bool save) {w.requestState(path,save);}
    static const auto& defaults(const MainWindow& w) { return w.romDefaults_; }
    template<class Edit>
    static void configure(MainWindow& w,Edit edit) {
        edit(w.romDefaults_);
        w.romDefaults_.normalize();
    }
    static RunnerState workerState(MainWindow& w) { return w.runner_.takeSnapshot().state; }
    static bool updateRom(MainWindow& w, RomUpdateRequest request) {
        if (!w.runner_.updateRom(std::move(request))) return false;
        w.operationStarted(RunnerOperation::UpdateRom);
        return true;
    }
};
}
using smsgg::win32::MainWindow;
using smsgg::win32::MainWindowTestAccess;
using smsgg::win32::RunnerState;
static_assert(std::is_base_of_v<WTL::CFrameWindowImpl<MainWindow>, MainWindow>,
    "MainWindow must use WTL frame window management");
namespace {
std::unique_ptr<smsgg::test::FakeAudioConsumer> audio;
std::unique_ptr<MainWindow> makeWindow(const std::filesystem::path& settingsPath={}) {
    smsgg::test::xinput::reset();
    audio=std::make_unique<smsgg::test::FakeAudioConsumer>();
    auto w = std::make_unique<MainWindow>(GetModuleHandleW(nullptr),audio->stream,settingsPath);
    MainWindowTestAccess::useTestGamepad(*w);
    MainWindowTestAccess::database(*w,std::nullopt);
    if(settingsPath.empty()) MainWindowTestAccess::useLegacyFixtureDefaults(*w);
    return w;
}
template<class Predicate>
void waitFor(MainWindow& window, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate(MainWindowTestAccess::snapshot(window))) {
        if (std::chrono::steady_clock::now() >= deadline || !window.pumpOnce(100))
            throw std::runtime_error("GUI condition timeout/early exit");
    }
}
std::filesystem::path fixture(const std::filesystem::path& root, bool header = false) {
    std::filesystem::create_directories(root);
    const auto path = root / (header ? L"header \u65e5\u672c\u8a9e.sms" : L"input \u65e5\u672c\u8a9e space.sms");
    auto bytes = smsgg::test::stage5InputRom();
    if (header) bytes.insert(bytes.begin(), 512, 0);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!out) throw std::runtime_error("GUI fixture write failed");
    return path;
}
bool pixel(const smsgg::win32::RunnerSnapshot& s, uint8_t r, uint8_t g) {
    const auto p = s.pixels[0];
    return s.frames >= 3 && p.r == r && p.g == g && p.b == 0;
}
void press(MainWindow& w, unsigned key, bool repeat = false) {
    SendMessageW(w.handle(), WM_KEYDOWN, key, repeat ? (LPARAM{1} << 30) : 0);
}
void release(MainWindow& w, unsigned key) { SendMessageW(w.handle(), WM_KEYUP, key, 0); }
void command(MainWindow& w, unsigned id) { SendMessageW(w.handle(), WM_COMMAND, id, 0); }
void configure(MainWindow& window,auto edit) { MainWindowTestAccess::configure(window,edit); }
bool closeWindow(MainWindow& window) {
    SendMessageW(window.handle(),WM_CLOSE,0,0);
    const auto end=GetTickCount64()+5000;
    while(window.handle() && GetTickCount64()<end) window.pumpOnce(100);
    return !window.handle() && MainWindowTestAccess::stopped(window);
}

bool fmOptions(const std::filesystem::path& root) {
    auto w=makeWindow();
    configure(*w,[](auto& d){d.fmEnabled=true;});
    w->openRom(fixture(root));
    waitFor(*w,[](const auto& s){return s.frames>=3 && s.fmEnabled;});
    wchar_t title[512]{};
    GetWindowTextW(w->handle(),title,512);
    if(std::wstring_view(title).find(L" | FM")==std::wstring_view::npos) return false;
    configure(*w,[](auto& d){d.fmEnabled=false;});
    if(!MainWindowTestAccess::snapshot(*w).fmEnabled) return false;
    w->openRom(fixture(root));
    waitFor(*w,[](const auto& s){return s.frames>=3 && !s.fmEnabled;});
    GetWindowTextW(w->handle(),title,512);
    if(std::wstring_view(title).find(L" | FM")!=std::wstring_view::npos) return false;
    closeWindow(*w);
    return MainWindowTestAccess::stopped(*w);
}

bool controls(const std::filesystem::path& root) {
    auto w = makeWindow();
    w->openRom(fixture(root));
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    if (!MainWindowTestAccess::color(*w, 255, 0)) return false;
    const auto beforeMute=MainWindowTestAccess::snapshot(*w).tstates;
    command(*w,IDM_MUTE);
    waitFor(*w,[&](const auto& s){return s.muted && s.tstates>beforeMute;});
    if(!(GetMenuState(GetMenu(w->handle()),IDM_MUTE,MF_BYCOMMAND)&MF_CHECKED)) return false;
    command(*w,IDM_MUTE);
    waitFor(*w,[](const auto& s){return !s.muted;});
    constexpr unsigned gameKeys[]{VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X'};
    for (const unsigned key : gameKeys) {
        press(*w, key);
        waitFor(*w, [](const auto& s) { return pixel(s, 0, 255); });
        if (!MainWindowTestAccess::color(*w, 0, 255)) return false;
        release(*w, key);
        waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    }
    SetFocus(w->handle());
    if (GetFocus() != w->handle()) return false;
    press(*w, 'Z');
    waitFor(*w, [](const auto& s) { return pixel(s, 0, 255); });
    std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)> other(
        CreateWindowExW(0, L"STATIC", L"Focus test", WS_CHILD, 0, 0, 1, 1,
            w->handle(), nullptr, GetModuleHandleW(nullptr), nullptr), &DestroyWindow);
    if (!other) return false;
    SetFocus(other.get()); // Real OS focus transition must release the held game button.
    if (GetFocus() != other.get()) return false;
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    other.reset();
    press(*w, VK_RETURN);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 255); });
    if (!MainWindowTestAccess::color(*w, 255, 255)) return false;
    press(*w, VK_RETURN, true);
    w->pumpOnce(100); // Observe a snapshot after delivering the repeated key event.
    const auto afterRepeat = MainWindowTestAccess::snapshot(*w).frames;
    waitFor(*w, [&](const auto& s) { return s.frames >= afterRepeat + 3; });
    if (!pixel(MainWindowTestAccess::snapshot(*w), 255, 255)) return false;
    release(*w, VK_RETURN);
    press(*w, VK_RETURN);
    release(*w, VK_RETURN);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    press(*w, VK_SPACE);
    release(*w, VK_SPACE);
    waitFor(*w, [](const auto& s) { return s.state == RunnerState::Paused; });
    const auto paused = MainWindowTestAccess::snapshot(*w).tstates;
    w->pumpOnce(50);
    if (MainWindowTestAccess::snapshot(*w).tstates != paused) return false;
    press(*w, 'Z');
    SendMessageW(w->handle(), WM_KILLFOCUS, 0, 0);
    command(*w, IDM_HOST_PAUSE);
    waitFor(*w, [&](const auto& s) { return s.tstates > paused && pixel(s, 255, 0); });
    if (!MainWindowTestAccess::color(*w, 255, 0)) return false;
    // Resize and a DPI message use actual Win32 geometry, not a core resize API.
    if (!SetWindowPos(w->handle(), nullptr, 0, 0, 500, 400, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return false;
    if (!MainWindowTestAccess::color(*w, 255, 0)) return false;
    RECT recommended{50, 60, 650, 510};
    SendMessageW(w->handle(), WM_DPICHANGED, MAKELONG(144, 144), reinterpret_cast<LPARAM>(&recommended));
    RECT actual{};
    if (!GetWindowRect(w->handle(), &actual) || actual.left != 50 || actual.top != 60
        || actual.right != 650 || actual.bottom != 510) return false;
    const auto beforeMinimize = MainWindowTestAccess::snapshot(*w).frames;
    ShowWindow(w->handle(), SW_SHOWMINNOACTIVE);
    if (!IsIconic(w->handle())) return false;
    waitFor(*w, [&](const auto& s) { return s.frames >= beforeMinimize + 3; });
    ShowWindow(w->handle(), SW_SHOWNOACTIVATE);
    w->pumpOnce(100);
    if (IsIconic(w->handle()) || !MainWindowTestAccess::color(*w, 255, 0)) return false;
    closeWindow(*w);
    return !IsWindow(w->handle()) && MainWindowTestAccess::stopped(*w) && !w->pumpOnce(0);
}

HWND dialogOwner{};
unsigned dismissedDialogs{};
std::wstring lastDialogText;
int sramChoice=IDCANCEL;
int registrationChoice=IDCANCEL;
int editChoice=IDOK;
int impactChoice=IDCANCEL;
int clearChoice=IDNO;
int stateAction=IDCANCEL;
int stateRow=-1;
int stateConfirm=IDNO;
int displayedStates{};
std::wstring selectedStateText;
std::wstring stateConfirmationText;
std::wstring clearDescription;
std::wstring currentSramDescription;
smsgg::win32::RomDatabase* changeStatesOnReview{};
bool changedStatesOnReview{};
std::filesystem::path removeOnRetry;
std::function<void(HWND)> settingsAction;
std::function<void(HWND)> shortcutsAction;
bool closeSettingsOnError{};
std::wstring settingsErrorText;
int cheatCloseChoice=IDCANCEL;
LRESULT CALLBACK dismissDialog(int code, WPARAM value, LPARAM data) {
    if (code == HCBT_ACTIVATE) {
        const auto window = reinterpret_cast<HWND>(value);
        wchar_t className[32]{};
        GetClassNameW(window, className, 32);
        const auto owner=GetWindow(window,GW_OWNER);
        if (std::wstring_view(className) == L"#32770" && (owner == dialogOwner ||
            (owner && GetWindow(owner,GW_OWNER)==dialogOwner))) {
            ++dismissedDialogs;
            lastDialogText.clear();
            EnumChildWindows(window,[](HWND child,LPARAM) -> BOOL {
                wchar_t type[32]{},text[4096]{};
                GetClassNameW(child,type,32);
                if(std::wstring_view(type)==L"Static") {GetWindowTextW(child,text,4096);lastDialogText+=text;lastDialogText+=L'\n';}
                return TRUE;
            },0);
            wchar_t caption[128]{};GetWindowTextW(window,caption,128);
            if(std::wstring_view(caption)==L"Settings") {
                if(auto action=std::exchange(settingsAction,{})) action(window);
            } else if(std::wstring_view(caption)==L"Player 1 XInput Shortcuts") {
                if(auto action=std::exchange(shortcutsAction,{})) action(window);
            } else if(closeSettingsOnError && owner!=dialogOwner) {
                settingsErrorText=lastDialogText;
                PostMessageW(window,WM_CLOSE,0,0);
                PostMessageW(owner,WM_COMMAND,IDCANCEL,0);
            } else if(std::wstring_view(caption)==L"Register ROM") {
                if(registrationChoice==IDOK) SendDlgItemMessageW(window,IDC_ROM_RAM,CB_SETCURSEL,3,0);
                PostMessageW(window,WM_COMMAND,registrationChoice,0);
            } else if(std::wstring_view(caption)==L"Edit ROM settings") {
                if(editChoice==IDC_ROM_CLEAR_SRAM) {
                    wchar_t capacity[128]{};
                    GetDlgItemTextW(window,IDC_ROM_SRAM_CAPACITY,capacity,128);
                    currentSramDescription=capacity;
                }
                SendDlgItemMessageW(window,IDC_ROM_RAM,CB_SETCURSEL,2,0);
                PostMessageW(window,WM_COMMAND,editChoice,0);
            } else if(std::wstring_view(caption)==L"Clear SRAM") {
                clearDescription=lastDialogText;
                editChoice=IDCANCEL;
                PostMessageW(window,WM_COMMAND,clearChoice,0);
                if(clearChoice==IDNO) PostMessageW(owner,WM_COMMAND,IDCANCEL,0);
            } else if(std::wstring_view(caption)==L"Review ROM changes") {
                wchar_t impact[4096]{};
                GetDlgItemTextW(window,IDC_ROM_IMPACT,impact,4096);
                lastDialogText=impact;
                if(auto* db=std::exchange(changeStatesOnReview,nullptr))
                    changedStatesOnReview=!db->execute("UPDATE states SET name='renamed while reviewing' WHERE name='delete me'");
                PostMessageW(window,WM_COMMAND,impactChoice,0);
            } else if(std::wstring_view(caption)==L"Manage states") {
                displayedStates=static_cast<int>(SendDlgItemMessageW(window,IDC_STATE_LIST,LB_GETCOUNT,0,0));
                selectedStateText.clear();
                if(stateRow>=0 && stateRow<displayedStates) {
                    auto length=SendDlgItemMessageW(window,IDC_STATE_LIST,LB_GETTEXTLEN,stateRow,0);
                    std::wstring row(static_cast<size_t>(length)+1,L'\0');
                    SendDlgItemMessageW(window,IDC_STATE_LIST,LB_GETTEXT,stateRow,reinterpret_cast<LPARAM>(row.data()));
                    row.resize(static_cast<size_t>(length));
                    selectedStateText=std::move(row);
                    SendDlgItemMessageW(window,IDC_STATE_LIST,LB_SETCURSEL,stateRow,0);
                    SendMessageW(window,WM_COMMAND,MAKEWPARAM(IDC_STATE_LIST,LBN_SELCHANGE),0);
                }
                SetDlgItemTextW(window,IDC_STATE_NAME,L"same name");
                PostMessageW(window,WM_COMMAND,stateAction,0);
            } else if(std::wstring_view(caption)==L"Overwrite state" || std::wstring_view(caption)==L"Delete state") {
                stateConfirmationText=lastDialogText;
                stateAction=IDCANCEL;
                PostMessageW(window,WM_COMMAND,stateConfirm,0);
                if(stateConfirm==IDNO) PostMessageW(owner,WM_COMMAND,IDCANCEL,0);
            } else if(std::wstring_view(caption)==L"Unsaved cheats") {
                PostMessageW(window,WM_COMMAND,cheatCloseChoice,0);
            } else if(std::wstring_view(caption)==L"Memory Search and Cheats") {
                // The test controls the modeless dialog directly.
            } else if(std::wstring_view(caption)==L"SRAM was not saved") {
                const auto choice=sramChoice;sramChoice=IDCANCEL;
                if(choice==IDTRYAGAIN && !removeOnRetry.empty()) {std::error_code error;std::filesystem::remove(removeOnRetry,error);removeOnRetry.clear();}
                PostMessageW(window,WM_COMMAND,choice,0);
            } else PostMessageW(window, WM_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, value, data);
}
bool selectionOptions(const std::filesystem::path& root) {
    auto w=makeWindow();
    configure(*w,[](auto& d){d.options.mapper=smsgg::MapperKind::Auto;d.machine.reset();});
    std::filesystem::create_directories(root);
    auto bytes=smsgg::test::codemastersHeaderFixture();
    const auto base=smsgg::test::stage5InputRom();std::copy(base.begin(),base.end(),bytes.begin());
    std::copy_n("TMR SEGA",8,bytes.begin()+0x7ff0);bytes[0x7fff]=0x4c;
    bytes[0x7fe6]=bytes[0x7fe7]=bytes[0x7fe8]=bytes[0x7fe9]=0;
    uint32_t sum=0;
    for(size_t i=0;i<bytes.size();i+=2) if(i<0x7ff0 || i>=0x8000) sum+=bytes[i]+256u*bytes[i+1];
    const auto checksum=static_cast<uint16_t>(sum),negative=static_cast<uint16_t>(0u-checksum);
    bytes[0x7fe6]=static_cast<uint8_t>(checksum);bytes[0x7fe7]=static_cast<uint8_t>(checksum>>8);
    bytes[0x7fe8]=static_cast<uint8_t>(negative);bytes[0x7fe9]=static_cast<uint8_t>(negative>>8);
    const auto path=root/"codemasters.sms";
    {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());if(!out)return false;}
    w->openRom(path);
    waitFor(*w,[](const auto& s){return s.frames>=3 && s.mapper==smsgg::MapperKind::Codemasters;});
    const auto& current=MainWindowTestAccess::snapshot(*w);
    if(current.machine!=smsgg::Machine::Sms || !current.romMetadata
        || current.romMetadata->mapperSelection!=smsgg::MapperSelection::CodemastersHeader || !pixel(current,255,0)) return false;
    wchar_t title[512]{};GetWindowTextW(w->handle(),title,512);
    if(std::wstring_view(title).find(L"Codemasters / auto")==std::wstring_view::npos) return false;
    configure(*w,[](auto& d){d.options.mapper=smsgg::MapperKind::Sega;});configure(*w,[](auto& d){d.machine=smsgg::Machine::GameGear;});
    if(MainWindowTestAccess::snapshot(*w).mapper!=smsgg::MapperKind::Codemasters) return false;
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    command(*w,IDM_HELP_ROM_INFO);
    if(!UnhookWindowsHookEx(hook)) return false;
    if(lastDialogText.find(L"CRC32")==std::wstring::npos || lastDialogText.find(L"Codemasters header")==std::wstring::npos
        || lastDialogText.find(L"matched")==std::wstring::npos || lastDialogText.find(L"Sega checksum: not checked")==std::wstring::npos) return false;
    configure(*w,[](auto& d){d.machine=smsgg::Machine::Sms;});command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    w->openRom(path);waitFor(*w,[](const auto& s){return s.romChanged && s.mapper==smsgg::MapperKind::Sega;});
    GetWindowTextW(w->handle(),title,512);
    if(std::wstring_view(title).find(L"Sega / explicit")==std::wstring_view::npos) return false;
    configure(*w,[](auto& d){d.options.mapper=smsgg::MapperKind::Auto;d.machine.reset();d.options.sramBytes=8192;});
    const auto ramBytes=smsgg::test::stage9CodemastersRom(false,true);
    const auto ramPath=root/"codemasters-ram.sms";
    {std::ofstream out(ramPath,std::ios::binary);out.write(reinterpret_cast<const char*>(ramBytes.data()),ramBytes.size());if(!out) return false;}
    w->openRom(ramPath);
    waitFor(*w,[&](const auto& s){return s.romChanged && s.romPath==ramPath;});
    if(MainWindowTestAccess::snapshot(*w).state!=RunnerState::Paused
        || MainWindowTestAccess::snapshot(*w).mapper!=smsgg::MapperKind::Codemasters) return false;
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.frames>=10 && pixel(s,255,0);});
    if(MainWindowTestAccess::snapshot(*w).machine!=smsgg::Machine::Sms
        || !pixel(MainWindowTestAccess::snapshot(*w),255,0)) return false;
    closeWindow(*w);return MainWindowTestAccess::stopped(*w);
}
bool options(const std::filesystem::path& root) {
    auto w = makeWindow();
    configure(*w,[](auto& d){
        d.options={smsgg::MapperKind::Sega,8192,smsgg::CopierHeader::Strip512};
        d.standard=smsgg::VideoStandard::Pal;
    });
    const auto path = fixture(root, true);
    w->openRom(path);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    if (MainWindowTestAccess::snapshot(*w).frames != MainWindowTestAccess::snapshot(*w).tstates / (313 * 228)) return false;
    configure(*w,[](auto& d){d.standard=smsgg::VideoStandard::Ntsc;}); // Next load only; current system must retain PAL timing.
    const auto before = MainWindowTestAccess::snapshot(*w).frames;
    waitFor(*w, [&](const auto& s) { return s.frames >= before + 3; });
    if (MainWindowTestAccess::snapshot(*w).frames != MainWindowTestAccess::snapshot(*w).tstates / (313 * 228)) return false;
    dialogOwner = w->handle();
    const auto hook = SetWindowsHookExW(WH_CBT, dismissDialog, nullptr, GetCurrentThreadId());
    if (!hook) return false;
    command(*w, IDM_FILE_OPEN); // Real IFileOpenDialog, dismissed as cancellation by this thread's hook.
    w->openRom(root / "missing.sms");
    waitFor(*w, [](const auto& s) { return !s.error.empty(); }); // Dismiss the one error MessageBox too.
    if (!UnhookWindowsHookEx(hook)) return false;
    const auto& snapshot = MainWindowTestAccess::snapshot(*w);
    if (snapshot.romPath != path || snapshot.state != RunnerState::Running || !MainWindowTestAccess::color(*w, 255, 0)) return false;
    closeWindow(*w);
    return MainWindowTestAccess::stopped(*w);
}

bool errors(const std::filesystem::path& root) {
    auto w = makeWindow();
    w->openRom(fixture(root));
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    // Real missing-menu failure, without assumptions about COM's initialization count.
    const auto attachedMenu = ::GetMenu(w->handle());
    if (!attachedMenu || !::SetMenu(w->handle(), nullptr)) return false;
    WTL::CMenu menu(attachedMenu);
    command(*w, IDM_HOST_PAUSE);
    std::string reason;
    try { w->pumpOnce(0); }
    catch (const std::exception& error) { reason = error.what(); }
    std::cerr << "Callback failure: " << reason << '\n';
    if (reason.find("GetMenu") == std::string::npos) return false;
    return MainWindowTestAccess::stopped(*w);
}
bool audioFailure(const std::filesystem::path& root) {
    auto w=makeWindow();w->openRom(fixture(root));
    waitFor(*w,[](const auto& s){return s.frames>=3;});
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    const auto before=dismissedDialogs;
    audio->stream.fail("Injected audio endpoint failure");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(w->handle() && std::chrono::steady_clock::now()<deadline) w->pumpOnce(100);
    if(!UnhookWindowsHookEx(hook)) return false;
    const auto& s=MainWindowTestAccess::snapshot(*w);
    return !w->handle() && s.fatal && s.state==RunnerState::Faulted
        && s.error=="Injected audio endpoint failure" && dismissedDialogs==before+1 && !w->pumpOnce(0);
}
bool systemKeys(const std::filesystem::path& root) {
    auto w = makeWindow();
    w->openRom(fixture(root));
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    auto systemRelease = [&](unsigned key) {
        SendMessageW(w->handle(), WM_SYSKEYUP, key, (LPARAM{1} << 31) | (LPARAM{1} << 30) | (LPARAM{1} << 29) | 1);
    };
    press(*w, 'Z');
    waitFor(*w, [](const auto& s) { return pixel(s, 0, 255); });
    systemRelease('Z');
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    press(*w, VK_RETURN);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 255); });
    systemRelease(VK_RETURN);
    press(*w, VK_RETURN);
    systemRelease(VK_RETURN);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    press(*w, VK_SPACE);
    waitFor(*w, [](const auto& s) { return s.state == RunnerState::Paused; });
    systemRelease(VK_SPACE);
    press(*w, VK_SPACE);
    systemRelease(VK_SPACE);
    waitFor(*w, [](const auto& s) { return s.state == RunnerState::Running; });
    closeWindow(*w);
    return MainWindowTestAccess::stopped(*w);
}
bool lifetime(const std::filesystem::path& root) {
    const auto path = fixture(root);
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        auto w = makeWindow();
        w->openRom(path);
        waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
        const auto handle = w->handle();
        if (!DestroyWindow(handle) || IsWindow(handle) || w->handle() != nullptr
            || !MainWindowTestAccess::stopped(*w) || w->pumpOnce(0)) return false;
    }
    HWND handle{};
    {
        auto w = makeWindow();
        w->openRom(path);
        waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
        handle = w->handle(); // No WM_CLOSE: destruction still owns shutdown and HWND cleanup.
    }
    MSG message{};
    return !IsWindow(handle) && PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)
        && message.message == WM_QUIT;
}
bool fullscreen() {
    auto w=makeWindow();
    const auto hwnd=w->handle();
    ShowWindow(hwnd,SW_SHOWNORMAL);
    const auto menu=GetMenu(hwnd);
    const auto style=GetWindowLongW(hwnd,GWL_STYLE)&WS_OVERLAPPEDWINDOW;
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if(!menu || GetMenuState(GetSubMenu(menu,1),IDM_FULLSCREEN,MF_BYCOMMAND)==UINT(-1) ||
       !style || !GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)) return false;
    auto altEnter=[&](bool repeat=false) {
        SendMessageW(hwnd,WM_SYSKEYDOWN,VK_RETURN,1|(LPARAM{1}<<29)|(repeat?(LPARAM{1}<<30):0));
    };
    auto coversMonitor=[&] {
        RECT client{};
        POINT origin{};
        return !GetMenu(hwnd) && !(GetWindowLongW(hwnd,GWL_STYLE)&WS_OVERLAPPEDWINDOW) &&
            GetClientRect(hwnd,&client) && ClientToScreen(hwnd,&origin) && OffsetRect(&client,origin.x,origin.y) &&
            EqualRect(&client,&monitor.rcMonitor);
    };
    const auto clientCursorHidden=[&] {
        SetCursor(LoadCursorW(nullptr,IDC_ARROW));
        SendMessageW(hwnd,WM_SETCURSOR,reinterpret_cast<WPARAM>(hwnd),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
        return GetCursor()==nullptr;
    };
    for(const int show:{SW_SHOWNORMAL,SW_MAXIMIZE}) {
        ShowWindow(hwnd,show);
        RECT before{},after{};
        if(!GetWindowRect(hwnd,&before)) return false;
        // A system-key message without the Alt context is not the shortcut.
        SendMessageW(hwnd,WM_SYSKEYDOWN,VK_RETURN,1);
        if(GetMenu(hwnd)!=menu) return false;
        altEnter();
        if(!coversMonitor() || IsZoomed(hwnd) || MainWindowTestAccess::pad(*w).start) return false;
        altEnter(true);
        press(*w,VK_RETURN,true); // Alt may be released before the held Enter.
        if(!coversMonitor() || MainWindowTestAccess::pad(*w).start) return false;
        release(*w,VK_RETURN);
        altEnter();
        SendMessageW(hwnd,WM_SYSKEYUP,VK_RETURN,1|(LPARAM{1}<<29)|(LPARAM{1}<<30)|(LPARAM{1}<<31));
        if(GetMenu(hwnd)!=menu || (GetWindowLongW(hwnd,GWL_STYLE)&WS_OVERLAPPEDWINDOW)!=style ||
           !GetWindowRect(hwnd,&after) || !EqualRect(&before,&after) || bool(IsZoomed(hwnd))!=(show==SW_MAXIMIZE))
            return false;
    }
    press(*w,VK_RETURN);
    if(!MainWindowTestAccess::pad(*w).start || GetMenu(hwnd)!=menu) return false;
    release(*w,VK_RETURN);
    command(*w,IDM_FULLSCREEN);
    if(!coversMonitor()) return false;
    SendMessageW(hwnd,WM_SETFOCUS,0,0);
    if(!clientCursorHidden()) { std::cerr<<"Fullscreen cursor is visible\n"; return false; }
    SendMessageW(hwnd,WM_KILLFOCUS,0,0);
    if(clientCursorHidden()) return false;
    SendMessageW(hwnd,WM_SETFOCUS,0,0);
    if(!clientCursorHidden()) return false;
    static bool popupCursorVisible,exitFromPopup;
    popupCursorVisible=false;
    exitFromPopup=false;
    const auto popupHook=SetWindowsHookExW(WH_CALLWNDPROC,[](int code,WPARAM sent,LPARAM data)->LRESULT {
        if(code>=0) {
            const auto& event=*reinterpret_cast<const CWPSTRUCT*>(data);
            if(event.message==WM_INITMENUPOPUP) {
                if(GetMenuItemID(reinterpret_cast<HMENU>(event.wParam),0)==IDM_FULLSCREEN) {
                    SendMessageW(event.hwnd,WM_SETCURSOR,reinterpret_cast<WPARAM>(event.hwnd),
                        MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
                    popupCursorVisible=GetCursor()!=nullptr;
                    if(exitFromPopup) {
                        PostMessageW(event.hwnd,WM_KEYDOWN,VK_DOWN,1);
                        PostMessageW(event.hwnd,WM_KEYDOWN,VK_RETURN,1);
                    }
                }
                PostMessageW(event.hwnd,WM_CANCELMODE,0,0);
            }
        }
        return CallNextHookEx(nullptr,code,sent,data);
    },nullptr,GetCurrentThreadId());
    if(!popupHook) return false;
    SendMessageW(hwnd,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(hwnd),-1);
    const bool dismissed=popupCursorVisible && coversMonitor() && clientCursorHidden();
    popupCursorVisible=false;
    exitFromPopup=true;
    SendMessageW(hwnd,WM_CONTEXTMENU,reinterpret_cast<WPARAM>(hwnd),-1);
    if(!UnhookWindowsHookEx(popupHook) || !dismissed || !popupCursorVisible ||
       GetMenu(hwnd)!=menu || !IsZoomed(hwnd) || clientCursorHidden()) return false;
    altEnter();
    if(!coversMonitor() || !closeWindow(*w)) return false;
    w.reset();
    return !IsMenu(menu);
}
bool aspect(const std::filesystem::path& root) {
    const auto rom=fixture(root);
    const auto settings=std::filesystem::absolute(root/"aspect.sqlite3");
    std::filesystem::remove(settings);
    auto w=makeWindow(settings);
    auto checked=[&](unsigned selected,unsigned other) {
        const auto menu=GetMenu(w->handle());
        const auto state=GetMenuState(menu,selected,MF_BYCOMMAND);
        const auto otherState=GetMenuState(menu,other,MF_BYCOMMAND);
        return state!=UINT(-1) && otherState!=UINT(-1) && (state&MF_CHECKED) && !(otherState&MF_CHECKED);
    };
    if(!checked(IDM_ASPECT_SQUARE,IDM_ASPECT_4_3)) return false;
    command(*w,IDM_ASPECT_4_3);
    if(!checked(IDM_ASPECT_4_3,IDM_ASPECT_SQUARE)) return false;
    configure(*w,[](auto& d){d.machine=smsgg::Machine::Sms;});
    w->openRom(rom);
    waitFor(*w,[](const auto& s){return s.frames>=3;});
    if(!checked(IDM_ASPECT_4_3,IDM_ASPECT_SQUARE)) return false;
    configure(*w,[](auto& d){d.machine=smsgg::Machine::GameGear;});
    w->openRom(rom);
    waitFor(*w,[](const auto& s){return s.machine==smsgg::Machine::GameGear;});
    if(!checked(IDM_ASPECT_4_3,IDM_ASPECT_SQUARE) || !closeWindow(*w)) return false;
    w.reset();
    // Quit messages belong to the previous window's pump.
    MSG message{};
    while(PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
    w=makeWindow(settings);
    return checked(IDM_ASPECT_4_3,IDM_ASPECT_SQUARE) && closeWindow(*w);
}
bool windowScale(const std::filesystem::path& root) {
    const auto rom=fixture(root);
    auto w=makeWindow();
    const auto hwnd=w->handle();
    ShowWindow(hwnd,SW_MAXIMIZE);
    auto sizeIs=[&](long width,long height) {
        RECT client{};
        if(GetClientRect(hwnd,&client) && client.right==width && client.bottom==height && !IsZoomed(hwnd))
            return true;
        std::cerr<<"Expected windowed "<<width<<'x'<<height<<", got "<<client.right<<'x'<<client.bottom
            <<", maximized="<<bool(IsZoomed(hwnd))<<'\n';
        return false;
    };
    press(*w,VK_F8);
    if(!sizeIs(256,192)) return false;
    press(*w,VK_F8,true);
    if(!sizeIs(256,192)) return false;
    release(*w,VK_F8);
    press(*w,VK_F8);
    if(!sizeIs(512,384)) return false;
    command(*w,IDM_FULLSCREEN);
    RECT before{},after{};
    if(!GetWindowRect(hwnd,&before)) return false;
    press(*w,VK_F8);
    if(!GetWindowRect(hwnd,&after) || !EqualRect(&before,&after)) return false;
    command(*w,IDM_FULLSCREEN);
    if(!sizeIs(512,384)) return false;

    // Cap the workspace at the measured x2 frame so x3 through x5 cannot fit.
    RECT workArea{};
    if(!GetWindowRect(hwnd,&workArea)) return false;
    MainWindowTestAccess::cycleWindowScale(*w,workArea);
    if(!sizeIs(256,192)) return false;
    if(!SetWindowPos(hwnd,nullptr,workArea.left+16,workArea.top+16,0,0,
        SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE)) return false;
    MainWindowTestAccess::cycleWindowScale(*w,workArea);
    if(!sizeIs(512,384) || !GetWindowRect(hwnd,&after) || !EqualRect(&workArea,&after)) return false;
    ShowWindow(hwnd,SW_MAXIMIZE);
    if(!GetWindowRect(hwnd,&before)) return false;
    MainWindowTestAccess::cycleWindowScale(*w,RECT{0,0,100,100});
    if(!IsZoomed(hwnd) || !GetWindowRect(hwnd,&after) || !EqualRect(&before,&after)) return false;

    configure(*w,[](auto& d){d.machine=smsgg::Machine::GameGear;});
    w->openRom(rom);
    waitFor(*w,[](const auto& s){return s.machine==smsgg::Machine::GameGear && s.frameChanged;});
    ShowWindow(hwnd,SW_MAXIMIZE);
    press(*w,VK_F8);
    if(!sizeIs(160,144)) return false;
    command(*w,IDM_ASPECT_4_3);
    ShowWindow(hwnd,SW_MAXIMIZE);
    press(*w,VK_F8);
    if(!sizeIs(192,144)) return false;
    press(*w,VK_F8);
    return sizeIs(384,288) && closeWindow(*w);
}
bool newRomDefaults(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    std::filesystem::create_directories(root);
    const auto dbPath=std::filesystem::absolute(root/"defaults.sqlite3");
    std::filesystem::remove(dbPath);
    {
        auto opened=RomDatabase::open(dbPath);
        auto& db=std::get<RomDatabase>(opened);
        RomDefaults rom;
        rom.options.sramBytes=16384;
        rom.fmPlugin=L"acesms-fm-ymfm.dll";
        rom.fmMethod=FmInterpolation::Linear;
        if(db.saveAppSettings(rom,defaultControllerConfiguration()) || db.saveAspectRatio(true)) return false;
    }
    auto w=makeWindow(dbPath);
    const auto fileMenu=GetSubMenu(GetMenu(w->handle()),0);
    if(GetMenuState(fileMenu,IDM_SETTINGS,MF_BYCOMMAND)==UINT(-1))
        throw std::runtime_error("Settings command is missing from File menu");
    if(GetMenuState(GetMenu(w->handle()),1242,MF_BYCOMMAND)!=UINT(-1))
        throw std::runtime_error("New ROM Defaults menu still exists");
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    const auto open=[&](std::function<void(HWND)> action) {
        const auto before=dismissedDialogs;
        settingsAction=std::move(action);
        command(*w,IDM_SETTINGS);
        settingsAction={};
        return dismissedDialogs>before;
    };
    const auto select=[](HWND dialog,int id,int at) {
        SendDlgItemMessageW(dialog,id,CB_SETCURSEL,at,0);
        SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(GetDlgItem(dialog,id)));
    };
    const auto selected=[](HWND dialog,int id){return SendDlgItemMessageW(dialog,id,CB_GETCURSEL,0,0);};
    const auto enabled=[](HWND dialog,int id){return IsWindowEnabled(GetDlgItem(dialog,id))!=0;};
    auto defaultsOk=true;
    if(!open([&](HWND dialog) {
        defaultsOk=selected(dialog,IDC_ROM_MACHINE)==3 && selected(dialog,IDC_ROM_MAPPER)==3 &&
            selected(dialog,IDC_ROM_RAM)==2 && selected(dialog,IDC_ROM_BACKEND)==1 &&
            selected(dialog,IDC_ROM_INTERPOLATION)==2 && enabled(dialog,IDC_ROM_FM) &&
            IsDlgButtonChecked(dialog,IDC_ROM_FM)==BST_CHECKED;
        select(dialog,IDC_ROM_VIDEO,1);
        defaultsOk=defaultsOk && !enabled(dialog,IDC_ROM_FM) && !enabled(dialog,IDC_ROM_BACKEND) &&
            IsDlgButtonChecked(dialog,IDC_ROM_FM)==BST_CHECKED;
        select(dialog,IDC_ROM_MACHINE,2);
        defaultsOk=defaultsOk && selected(dialog,IDC_ROM_VIDEO)==0 && selected(dialog,IDC_ROM_MAPPER)==0 &&
            selected(dialog,IDC_ROM_RAM)==0 && !enabled(dialog,IDC_ROM_VIDEO) &&
            !enabled(dialog,IDC_ROM_MAPPER) && !enabled(dialog,IDC_ROM_RAM);
        select(dialog,IDC_ROM_MACHINE,3);
        defaultsOk=defaultsOk && selected(dialog,IDC_ROM_VIDEO)==1 && selected(dialog,IDC_ROM_MAPPER)==3 &&
            selected(dialog,IDC_ROM_RAM)==2;
        select(dialog,IDC_ROM_MAPPER,2);
        defaultsOk=defaultsOk && selected(dialog,IDC_ROM_RAM)==0 &&
            SendDlgItemMessageW(dialog,IDC_ROM_RAM,CB_GETCOUNT,0,0)==2;
        select(dialog,IDC_ROM_MACHINE,1);
        defaultsOk=defaultsOk && selected(dialog,IDC_ROM_VIDEO)==0 && !enabled(dialog,IDC_ROM_VIDEO);
        PostMessageW(dialog,WM_CLOSE,0,0);
    }) || !defaultsOk || MainWindowTestAccess::defaults(*w).options.sramBytes!=16384 ||
        MainWindowTestAccess::defaults(*w).machine) throw std::runtime_error("settings values or cancel");

    if(!open([&](HWND dialog) {
        select(dialog,IDC_ROM_RAM,3);
        select(dialog,IDC_ROM_COPIER_HEADER,1);
        PostMessageW(dialog,WM_COMMAND,IDOK,0);
    }) || MainWindowTestAccess::defaults(*w).options.sramBytes!=32768) return false;
    {
        auto opened=RomDatabase::open(dbPath);
        const auto saved=std::get<AppSettings>(std::get<RomDatabase>(opened).readAppSettings());
        if(!saved.fourThree || saved.rom.options.sramBytes!=32768 || saved.rom.options.copierHeader!=CopierHeader::Strip512 ||
            saved.rom.fmMethod!=FmInterpolation::Linear) return false;
    }
    const auto badDb=root/"missing-parent"/"acesms.sqlite3";
    if(!closeWindow(*w)) return false;
    w.reset();
    MSG message{};
    while(PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
    w=makeWindow(dbPath);
    dialogOwner=w->handle();
    if(MainWindowTestAccess::defaults(*w).options.sramBytes!=32768 ||
        MainWindowTestAccess::defaults(*w).options.copierHeader!=CopierHeader::Strip512) return false;

    if(!open([&](HWND dialog) {
        select(dialog,IDC_ROM_MACHINE,0);
        PostMessageW(dialog,WM_COMMAND,IDOK,0);
    })) return false;
    w->openRom(fixture(root,true));
    waitFor(*w,[](const auto& s){return !s.busy && s.operationSerial>0;});
    if(!MainWindowTestAccess::snapshot(*w).operationSucceeded)
        throw std::runtime_error(MainWindowTestAccess::snapshot(*w).error);
    waitFor(*w,[](const auto& s){return s.frames>=3 && s.fmEnabled;});
    bool inputReleased=false;
    press(*w,'Z');
    if(!open([&](HWND dialog) {
        const auto end=GetTickCount64()+3000;
        while(MainWindowTestAccess::workerState(*w)!=RunnerState::Paused && GetTickCount64()<end) Sleep(1);
        press(*w,'Z',true);
        inputReleased=MainWindowTestAccess::workerState(*w)==RunnerState::Paused &&
            !MainWindowTestAccess::pad(*w).button1;
        PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
    }) || !inputReleased) throw std::runtime_error("running session was not paused with neutral input");
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Running;});
    press(*w,'Z',true);
    if(MainWindowTestAccess::pad(*w).button1) return false;
    release(*w,'Z');
    if(!closeWindow(*w)) return false;
    w.reset();
    while(PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}

    w=makeWindow(badDb);
    dialogOwner=w->handle();
    closeSettingsOnError=true;
    const auto opened=open([&](HWND dialog) {
        select(dialog,IDC_ROM_RAM,1);
        PostMessageW(dialog,WM_COMMAND,IDOK,0);
    });
    closeSettingsOnError=false;
    if(!opened || settingsErrorText.find(badDb.wstring())==std::wstring::npos ||
        MainWindowTestAccess::defaults(*w).options.sramBytes!=32768 || std::filesystem::exists(badDb))
        throw std::runtime_error("could not verify failed settings write");
    return closeWindow(*w);
}
struct AssignmentCleanupProbe {
    static inline AssignmentCleanupProbe* active{};
    static constexpr MSG probeKey{nullptr,WM_KEYDOWN,VK_ESCAPE};
    unsigned destroyed{},stillAttached{},installed{};
    bool failInitialization{};

    static LRESULT CALLBACK observe(HWND field,UINT message,WPARAM key,LPARAM detail,UINT_PTR id,DWORD_PTR owner) {
        if (message==WM_GETDLGCODE && detail==reinterpret_cast<LPARAM>(&probeKey)) return 0;
        if (message==WM_NCDESTROY) {
            auto& probe=*reinterpret_cast<AssignmentCleanupProbe*>(owner);
            ++probe.destroyed;
            // A nested query reaches assignmentKey only if it is still registered.
            if (SendMessageW(field,WM_GETDLGCODE,VK_ESCAPE,reinterpret_cast<LPARAM>(&probeKey))&DLGC_WANTMESSAGE)
                ++probe.stillAttached;
            RemoveWindowSubclass(field,observe,id);
        }
        return DefSubclassProc(field,message,key,detail);
    }
    static LRESULT CALLBACK beforeInitialize(int code,WPARAM wParam,LPARAM data) {
        if (code>=0) {
            const auto& event=*reinterpret_cast<const CWPSTRUCT*>(data);
            if (event.message==WM_INITDIALOG && GetDlgItem(event.hwnd,IDC_SETTINGS_TABS)) {
                auto& probe=*active;
                for (const auto id:{IDC_PAD_KEY_BASE,IDC_PAD_BUTTON_BASE})
                    if(SetWindowSubclass(GetDlgItem(event.hwnd,id),observe,1,reinterpret_cast<DWORD_PTR>(&probe)))
                        ++probe.installed;
                if (probe.failInitialization) DestroyWindow(GetDlgItem(event.hwnd,IDC_PAD_BUTTON_BASE+1));
            }
        }
        return CallNextHookEx(nullptr,code,wParam,data);
    }
};

bool settingsRecovery(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    std::filesystem::create_directories(root);
    const auto path = root / "recovery.sqlite3";
    const std::filesystem::path absent = L"acesms-settings-missing.dll";
    auto opened = RomDatabase::open(path);
    auto& db = std::get<RomDatabase>(opened);
    RomDefaults settings;
    settings.machine = Machine::Sms;
    settings.options.mapper = MapperKind::RomOnly;
    settings.options.sramBytes = 0;
    settings.fmPlugin = absent;
    if (db.saveAppSettings(settings, defaultControllerConfiguration())) return false;
    auto w = makeWindow(path);
    if (MainWindowTestAccess::defaults(*w).fmPlugin != absent)
        throw std::runtime_error("Unavailable FM plugin was discarded on startup");

    dialogOwner = w->handle();
    const auto hook = SetWindowsHookExW(WH_CBT, dismissDialog, nullptr, GetCurrentThreadId());
    if (!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    AssignmentCleanupProbe cleanup;
    const auto observe=SetWindowsHookExW(WH_CALLWNDPROC,AssignmentCleanupProbe::beforeInitialize,nullptr,GetCurrentThreadId());
    if (!observe) return false;
    Unhook unhookProbe{observe};
    AssignmentCleanupProbe::active=&cleanup;
    bool refused = false;
    settingsAction = [&](HWND dialog) {
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        wchar_t text[256]{};
        GetDlgItemTextW(dialog, IDC_ROM_ERROR, text, 256);
        refused = std::wstring_view(text).find(L"FM DLL") != std::wstring_view::npos;
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    };
    command(*w, IDM_SETTINGS);
    if (!refused || std::get<AppSettings>(db.readAppSettings()).rom.fmPlugin != absent)
        throw std::runtime_error("Unavailable FM selection changed the settings");

    settingsAction = [](HWND dialog) {
        SendDlgItemMessageW(dialog, IDC_ROM_BACKEND, CB_SETCURSEL, 0, 0);
        PostMessageW(dialog, WM_COMMAND, IDOK, 0);
    };
    command(*w, IDM_SETTINGS);
    if (!std::get<AppSettings>(db.readAppSettings()).rom.fmPlugin.empty()) return false;
    w->openRom(fixture(root));
    waitFor(*w, [](const auto& s) { return s.operationSucceeded && s.state == RunnerState::Running; });

    cleanup.failInitialization=true;
    lastDialogText.clear();
    command(*w, IDM_SETTINGS);
    cleanup.failInitialization=false;
    if (lastDialogText.find(L"Subclass button field") == std::wstring::npos)
        throw std::runtime_error("Settings initialization failure was not reported");
    waitFor(*w, [](const auto& s) { return s.state == RunnerState::Running; });
    bool reopened = false;
    settingsAction = [&](HWND dialog) {
        reopened = (SendDlgItemMessageW(dialog,IDC_PAD_KEY_BASE,WM_GETDLGCODE,VK_ESCAPE,
            reinterpret_cast<LPARAM>(&AssignmentCleanupProbe::probeKey))&DLGC_WANTMESSAGE)!=0;
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    };
    command(*w, IDM_SETTINGS);
    const auto closed = closeWindow(*w);
    MSG message{};
    while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    if (cleanup.installed!=8 || cleanup.destroyed!=8)
        throw std::runtime_error("Settings test did not observe both input fields on all four dialog exits");
    if (cleanup.stillAttached)
        throw std::runtime_error("Settings assignment subclass still registered during child destruction");
    return reopened && closed;
}

bool controllerSettings(const std::filesystem::path& root) {
    using namespace smsgg::win32;
    namespace api=smsgg::test::xinput;
    std::filesystem::create_directories(root);
    const auto dbPath=root/L"controller.sqlite3";
    std::filesystem::remove(dbPath);
    auto w=makeWindow(dbPath);
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    const auto select=[](HWND dialog,int id,int index) {
        SendDlgItemMessageW(dialog,id,CB_SETCURSEL,index,0);
        SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(dialog,id)));
    };
    const auto openControllerTab=[](HWND dialog) {
        const auto tab=GetDlgItem(dialog,IDC_SETTINGS_TABS);
        if(!tab || TabCtrl_GetItemCount(tab)!=2) return false;
        TabCtrl_SetCurSel(tab,1);
        NMHDR event{tab,IDC_SETTINGS_TABS,static_cast<UINT>(TCN_SELCHANGE)};
        SendMessageW(dialog,WM_NOTIFY,IDC_SETTINGS_TABS,reinterpret_cast<LPARAM>(&event));
        return true;
    };
    const auto text=[](HWND dialog,int id) {
        wchar_t label[128]{};
        GetDlgItemTextW(dialog,id,label,_countof(label));
        return std::wstring(label);
    };
    bool correct=true;
    settingsAction=[&](HWND dialog) {
        if(!openControllerTab(dialog)) {
            correct=false;
            PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
            return;
        }
        api::results[0]=api::results[2]=ERROR_SUCCESS;
        BYTE originalKeys[256]{}, simulatedKeys[256]{};
        simulatedKeys[VK_KANJI]=0x80;
        if(!GetKeyboardState(originalKeys) || !SetKeyboardState(simulatedKeys)) {
            correct=false;
            PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
            return;
        }
        const auto keyField=GetDlgItem(dialog,IDC_PAD_KEY_BASE);
        SetFocus(keyField);
        SendMessageW(keyField,WM_KEYDOWN,VK_F5,0);
        correct=GetFocus()==keyField;
        SendMessageW(keyField,WM_KEYDOWN,VK_ESCAPE,0);
        correct=correct && GetFocus()==GetDlgItem(dialog,IDC_PAD_SOURCE);
        SetFocus(keyField);
        simulatedKeys[VK_SHIFT]=0x80;
        correct=SetKeyboardState(simulatedKeys) && correct;
        SendMessageW(keyField,WM_KEYDOWN,'U',0);
        const bool ignoredChord=GetFocus()==keyField;
        simulatedKeys[VK_SHIFT]=0;
        correct=SetKeyboardState(simulatedKeys) && correct;
        constexpr unsigned bindings[]{'U','D','L','R','F','G'};
        for(size_t i=0;i<std::size(bindings);++i) {
            const auto field=GetDlgItem(dialog,IDC_PAD_KEY_BASE+i);
            correct=correct && GetFocus()==field;
            SendMessageW(field,WM_KEYDOWN,bindings[i],0);
            if(i==0 && (!ignoredChord || GetFocus()!=GetDlgItem(dialog,IDC_PAD_KEY_BASE+1))) {
                std::cerr << "Shift must block assignment and VK_KANJI alone must allow it\n";
                correct=false;
            }
        }
        const auto rapidKey1=GetDlgItem(dialog,IDC_PAD_KEY_BASE+6);
        correct=correct && GetFocus()==rapidKey1;
        SendMessageW(rapidKey1,WM_KEYDOWN,'H',0);
        correct=correct && GetFocus()==GetDlgItem(dialog,IDC_PAD_KEY_BASE+7);
        correct=SetKeyboardState(originalKeys) && correct;
        SendMessageW(dialog,WM_COMMAND,IDC_PAD_CLEAR_RAPID_BASE+1,0);
        correct=correct && text(dialog,IDC_PAD_KEY_BASE+7)==L"(Unassigned)";
        select(dialog,IDC_PAD_PLAYER,1);
        select(dialog,IDC_PAD_SOURCE,2);
        select(dialog,IDC_PAD_SLOT,2);
        correct=correct && text(dialog,IDC_PAD_STATUS)==L"Slot 3 connected";
        const auto button1=GetDlgItem(dialog,IDC_PAD_BUTTON_BASE);
        const auto tick=[&] { SendMessageW(dialog,WM_TIMER,1,0); };
        SetFocus(button1);
        api::states[0].Gamepad.wButtons=XINPUT_GAMEPAD_Y;tick();
        api::states[2].Gamepad.wButtons=XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_A;tick();
        api::states[2].Gamepad.wButtons=XINPUT_GAMEPAD_Y;tick();
        correct=correct && GetFocus()==button1 && SendMessageW(button1,CB_GETCURSEL,0,0)==0;
        api::states[2].Gamepad.wButtons=0;tick();
        api::states[2].Gamepad.wButtons=XINPUT_GAMEPAD_Y;tick();
        correct=correct && GetFocus()==GetDlgItem(dialog,IDC_PAD_BUTTON_BASE+1)
            && SendMessageW(button1,CB_GETCURSEL,0,0)==3;
        select(dialog,IDC_PAD_BUTTON_BASE+1,4);
        const auto rapidButton1=GetDlgItem(dialog,IDC_PAD_BUTTON_BASE+2);
        SetFocus(rapidButton1);
        api::states[2].Gamepad.wButtons=XINPUT_GAMEPAD_Y;tick();
        correct=correct && GetFocus()==rapidButton1; // A held button cannot bind the next field.
        api::states[2].Gamepad.wButtons=0;tick();
        api::states[2].Gamepad.wButtons=XINPUT_GAMEPAD_RIGHT_SHOULDER;tick();
        correct=correct && GetFocus()==GetDlgItem(dialog,IDC_PAD_BUTTON_BASE+3)
            && SendMessageW(rapidButton1,CB_GETCURSEL,0,0)==5;
        select(dialog,IDC_PAD_BUTTON_BASE+3,8);
        select(dialog,IDC_PAD_PLAYER,0);
        select(dialog,IDC_PAD_SOURCE,1);
        select(dialog,IDC_PAD_SLOT,2);
        SendMessageW(dialog,WM_COMMAND,IDOK,0);
        correct=correct && text(dialog,IDC_PAD_ERROR).find(L"different slot")!=std::wstring::npos
            ;
        select(dialog,IDC_PAD_SLOT,0);
        shortcutsAction=[&](HWND shortcuts) {
            correct=correct && SendDlgItemMessageW(shortcuts,IDC_SHORTCUT_BASE,CB_GETCURSEL,0,0)==4 &&
                SendDlgItemMessageW(shortcuts,IDC_SHORTCUT_BASE+2,CB_GETCURSEL,0,0)==8;
            select(shortcuts,IDC_SHORTCUT_BASE,0); // A conflicts with Player 1 Button 1.
            PostMessageW(shortcuts,WM_COMMAND,IDOK,0);
        };
        SendMessageW(dialog,WM_COMMAND,IDC_PAD_SHORTCUTS,0);
        SendMessageW(dialog,WM_COMMAND,IDOK,0);
        correct=correct && text(dialog,IDC_PAD_ERROR).find(L"Save state")!=std::wstring::npos;
        shortcutsAction=[&](HWND shortcuts) {
            select(shortcuts,IDC_SHORTCUT_BASE,10); // Unassigned.
            select(shortcuts,IDC_SHORTCUT_BASE+1,4);
            PostMessageW(shortcuts,WM_COMMAND,IDOK,0);
        };
        SendMessageW(dialog,WM_COMMAND,IDC_PAD_SHORTCUTS,0);
        select(dialog,IDC_PAD_SOURCE,0);
        PostMessageW(dialog,WM_COMMAND,IDOK,0);
    };
    command(*w,IDM_SETTINGS);
    if(!correct || !std::filesystem::exists(dbPath)) throw std::runtime_error("Controller assignments or slot validation failed");
    const auto configured=[&] {
        SendMessageW(w->handle(),WM_SETFOCUS,0,0);
        api::results[2]=ERROR_SUCCESS;
        api::states[2].Gamepad.wButtons=0;
        MainWindowTestAccess::pollGamepad(*w);
        press(*w,'F');press(*w,'H');press(*w,'S');
        api::states[2].Gamepad.wButtons=XINPUT_GAMEPAD_Y|XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER;
        MainWindowTestAccess::pollGamepad(*w);
        const auto first=MainWindowTestAccess::pad(*w),second=MainWindowTestAccess::pad(*w,1);
        const bool rapid=MainWindowTestAccess::rapid(*w,0)==1 && MainWindowTestAccess::rapid(*w,1)==1;
        release(*w,'F');release(*w,'H');release(*w,'S');
        return first.button1 && !first.button2 && second.button1 && second.button2 && rapid;
    };
    if(!configured()) throw std::runtime_error("Controller assignments did not apply immediately");
    settingsAction=[&](HWND dialog) {
        openControllerTab(dialog);
        select(dialog,IDC_PAD_PLAYER,1);
        select(dialog,IDC_PAD_SOURCE,0);
        PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);
    };
    command(*w,IDM_SETTINGS);
    if(!configured() || !closeWindow(*w)) return false;
    w.reset();
    MSG message{};
    while(PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
    w=makeWindow(dbPath);
    dialogOwner=w->handle();
    if(!configured()) throw std::runtime_error("Controller configuration did not survive restart");
    auto opened=RomDatabase::open(dbPath);
    const auto saved=std::get<AppSettings>(std::get<RomDatabase>(opened).readAppSettings());
    if(saved.shortcuts[0]!=0 || saved.shortcuts[1]!=XINPUT_GAMEPAD_LEFT_SHOULDER) return false;
    return closeWindow(*w);
}
bool gg(const std::filesystem::path& root) {
    using smsgg::Machine;
    const auto smsPath=fixture(root),ggPath=root/"colors.gg";
    {const auto rom=smsgg::test::stage7GameGearRom();std::ofstream file(ggPath,std::ios::binary);
        file.write(reinterpret_cast<const char*>(rom.data()),rom.size());if(!file) return false;}
    auto w=makeWindow();configure(*w,[](auto& d){d.standard=smsgg::VideoStandard::Pal;});w->openRom(smsPath);
    waitFor(*w,[](const auto& s){return pixel(s,255,0);});
    configure(*w,[](auto& d){d.machine=smsgg::Machine::GameGear;});
    if(MainWindowTestAccess::snapshot(*w).machine!=Machine::Sms) return false;
    w->openRom(ggPath);
    auto ggColor=[](const auto& s,bool start) {
        const auto p=s.pixels[24*256+48];
        return s.machine==Machine::GameGear && s.frames>=4 && p.r==(start?238:204) && p.g==(start?204:102) && p.b==(start?170:17);
    };
    waitFor(*w,[&](const auto& s){return ggColor(s,false);});
    const auto p=MainWindowTestAccess::firstDisplayed(*w);
    if(p.r!=204 || p.g!=102 || p.b!=17) return false;
    wchar_t title[512]{};GetWindowTextW(w->handle(),title,512);
    if(std::wstring_view(title).find(L" | GG | ")==std::wstring_view::npos) return false;
    press(*w,VK_RETURN);waitFor(*w,[&](const auto& s){return ggColor(s,true);});
    const auto frame=MainWindowTestAccess::snapshot(*w).frames;
    press(*w,VK_RETURN,true);
    waitFor(*w,[&](const auto& s){return s.frames>=frame+4;});
    if(!ggColor(MainWindowTestAccess::snapshot(*w),true)) return false;
    release(*w,VK_RETURN);waitFor(*w,[&](const auto& s){return ggColor(s,false);});
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    const auto pausedTime=MainWindowTestAccess::snapshot(*w).tstates;
    press(*w,VK_RETURN);w->pumpOnce(0);
    if(MainWindowTestAccess::snapshot(*w).tstates!=pausedTime) return false;
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[&](const auto& s){return ggColor(s,true);});
    release(*w,VK_RETURN);waitFor(*w,[&](const auto& s){return ggColor(s,false);});
    namespace api=smsgg::test::xinput;
    MainWindowTestAccess::useGamepadInput(*w);
    api::results[0]=ERROR_SUCCESS;
    SendMessageW(w->handle(),WM_SETFOCUS,0,0);MainWindowTestAccess::pollGamepad(*w);
    api::states[0].Gamepad.wButtons=XINPUT_GAMEPAD_START;MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w,[&](const auto& s){return ggColor(s,true);});
    SendMessageW(w->handle(),WM_KILLFOCUS,0,0);MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w,[&](const auto& s){return ggColor(s,false);});
    SendMessageW(w->handle(),WM_SETFOCUS,0,0);MainWindowTestAccess::pollGamepad(*w);
    if(MainWindowTestAccess::pad(*w).start) return false;
    api::states[0].Gamepad.wButtons=0;MainWindowTestAccess::pollGamepad(*w);
    api::states[0].Gamepad.wButtons=XINPUT_GAMEPAD_START;MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w,[&](const auto& s){return ggColor(s,true);});
    api::results[0]=ERROR_DEVICE_NOT_CONNECTED;MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w,[&](const auto& s){return ggColor(s,false);});
    configure(*w,[](auto& d){d.machine=smsgg::Machine::Sms;}); // Next load selection cannot reinterpret current GG pixels/input.
    MainWindowTestAccess::useGamepadInput(*w,false);
    press(*w,VK_RETURN);waitFor(*w,[&](const auto& s){return ggColor(s,true);});release(*w,VK_RETURN);
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    w->openRom(root/"absent.sms");waitFor(*w,[](const auto& s){return !s.error.empty();});
    if(!UnhookWindowsHookEx(hook)) return false;
    if(MainWindowTestAccess::snapshot(*w).machine!=Machine::GameGear || MainWindowTestAccess::snapshot(*w).displayArea.width!=160) return false;
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    w->openRom(ggPath); // GG header/extension cannot override explicit SMS selection.
    waitFor(*w,[](const auto& s){return s.romChanged && s.state==RunnerState::Paused;});
    if(MainWindowTestAccess::snapshot(*w).machine!=Machine::Sms || MainWindowTestAccess::snapshot(*w).displayArea.width!=256) return false;
    configure(*w,[](auto& d){d.standard=smsgg::VideoStandard::Pal;});w->openRom(smsPath);
    waitFor(*w,[](const auto& s){return s.romChanged && s.state==RunnerState::Paused;});
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return pixel(s,255,0);});
    if(!MainWindowTestAccess::color(*w,255,0)) return false;
    closeWindow(*w);return MainWindowTestAccess::stopped(*w);
}
bool xinput(const std::filesystem::path& root) {
    namespace api = smsgg::test::xinput;
    auto w = makeWindow();
    MainWindowTestAccess::useGamepadInput(*w);
    api::results[0] = ERROR_SUCCESS;
    auto& buttons = api::states[0].Gamepad.wButtons;
    w->openRom(fixture(root));
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    SendMessageW(w->handle(), WM_SETFOCUS, 0, 0);
    MainWindowTestAccess::pollGamepad(*w); // Neutral baseline arms edge inputs.
    for (const WORD button : {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B}) {
        buttons = button;
        MainWindowTestAccess::pollGamepad(*w);
        waitFor(*w, [](const auto& s) { return pixel(s, 0, 255); });
        if (!MainWindowTestAccess::color(*w, 0, 255)) return false;
        buttons = 0;
        MainWindowTestAccess::pollGamepad(*w);
        waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    }
    buttons = XINPUT_GAMEPAD_START;
    MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 255); });
    const auto frame = MainWindowTestAccess::snapshot(*w).frames;
    MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [&](const auto& s) { return s.frames >= frame + 3; });
    if (!pixel(MainWindowTestAccess::snapshot(*w), 255, 255)) return false;
    buttons = 0; MainWindowTestAccess::pollGamepad(*w);
    buttons = XINPUT_GAMEPAD_START; MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    buttons = XINPUT_GAMEPAD_BACK; MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return s.state == RunnerState::Paused; });
    const auto time = MainWindowTestAccess::snapshot(*w).tstates;
    const auto calls = api::calls.size();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // No injected WM_TIMER: the real window timer must wake an otherwise paused UI.
    while (api::calls.size() == calls) {
        if (std::chrono::steady_clock::now() >= deadline || !w->pumpOnce(100)) return false;
    }
    if (MainWindowTestAccess::snapshot(*w).state != RunnerState::Paused
        || MainWindowTestAccess::snapshot(*w).tstates != time) return false;
    buttons |= XINPUT_GAMEPAD_START; MainWindowTestAccess::pollGamepad(*w); // No queued NMI while paused.
    buttons = XINPUT_GAMEPAD_START; MainWindowTestAccess::pollGamepad(*w);
    buttons |= XINPUT_GAMEPAD_BACK; MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return s.state == RunnerState::Running && pixel(s, 255, 0); });
    buttons = XINPUT_GAMEPAD_A;
    MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return pixel(s, 0, 255); });
    SendMessageW(w->handle(), WM_KILLFOCUS, 0, 0);
    buttons |= XINPUT_GAMEPAD_START;
    MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    SendMessageW(w->handle(), WM_SETFOCUS, 0, 0);
    MainWindowTestAccess::pollGamepad(*w); // Held Start after focus regain must not issue NMI.
    if(MainWindowTestAccess::pad(*w).button1 || MainWindowTestAccess::pad(*w).start) return false;
    buttons=0;MainWindowTestAccess::pollGamepad(*w);
    buttons=XINPUT_GAMEPAD_A;MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [](const auto& s) { return pixel(s, 0, 255); });
    press(*w, 'X');
    api::results[0] = ERROR_DEVICE_NOT_CONNECTED;
    MainWindowTestAccess::pollGamepad(*w);
    const auto pad = MainWindowTestAccess::pad(*w);
    if (pad.button1 || pad.button2) return false;
    release(*w, 'X');
    waitFor(*w, [](const auto& s) { return pixel(s, 255, 0); });
    closeWindow(*w);
    return MainWindowTestAccess::stopped(*w);
}
bool enabled(MainWindow& w,unsigned id) {
    const auto state=GetMenuState(GetMenu(w.handle()),id,MF_BYCOMMAND);
    return state!=UINT(-1) && !(state&(MF_DISABLED|MF_GRAYED));
}
void waitOperation(MainWindow& w,uint64_t previous) {
    waitFor(w,[&](const auto& s){return !s.busy && s.operationSerial>previous;});
}
std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);return {(std::istreambuf_iterator<char>(file)),{}};
}
void writeBytes(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if(!file) throw std::runtime_error("Persistence fixture write failed");
}
bool persistence(const std::filesystem::path& root) {
    auto w=makeWindow();
    for(auto id:{IDM_SAVE_STATE,IDM_LOAD_STATE}) if(enabled(*w,id)) return false;
    configure(*w,[](auto& d){d.options.mapper=smsgg::MapperKind::RomOnly;});command(*w,IDM_HOST_PAUSE);w->openRom(fixture(root));
    if(enabled(*w,IDM_FILE_OPEN)) return false;
    waitFor(*w,[](const auto& s){return s.romChanged && !s.busy;});
    if(enabled(*w,IDM_SAVE_STATE) || enabled(*w,IDM_LOAD_STATE)) return false;
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    command(*w,IDM_MUTE);waitFor(*w,[](const auto& s){return s.muted;});
    auto saved=std::make_unique<smsgg::win32::RunnerSnapshot>(MainWindowTestAccess::snapshot(*w));
    const auto state=root/L"saved \u65e5\u672c\u8a9e.state";MainWindowTestAccess::stateFile(*w,state,true);
    if(enabled(*w,IDM_SAVE_STATE) || enabled(*w,IDM_LOAD_STATE) || enabled(*w,IDM_FILE_OPEN)) return false;
    waitOperation(*w,saved->operationSerial);
    if(!std::filesystem::exists(state) || MainWindowTestAccess::snapshot(*w).tstates!=saved->tstates) return false;
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[&](const auto& s){return s.frames>=saved->frames+3;});
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;press(*w,'Z');
    MainWindowTestAccess::stateFile(*w,state,false);waitOperation(*w,serial);
    const auto& restored=MainWindowTestAccess::snapshot(*w);wchar_t title[512]{};GetWindowTextW(w->handle(),title,512);
    if(restored.tstates!=saved->tstates || restored.frames!=saved->frames || !restored.muted || restored.state!=RunnerState::Paused
        || MainWindowTestAccess::pad(*w).button1 || !MainWindowTestAccess::color(*w,255,0) || std::wstring_view(title).find(L"State loaded")==std::wstring_view::npos) return false;
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());if(!hook) return false;
    auto corrupt=readBytes(state);corrupt.back()^=1;writeBytes(root/L"bad.state",corrupt);
    MainWindowTestAccess::stateFile(*w,root/L"bad.state",false);waitOperation(*w,restored.operationSerial);
    const bool held=MainWindowTestAccess::snapshot(*w).tstates==saved->tstates && lastDialogText.find(L"checksum")!=std::wstring::npos;
    if(!UnhookWindowsHookEx(hook) || !held) return false;
    return closeWindow(*w);
}
bool stateDialogs(const std::filesystem::path& root) {
    auto w=makeWindow();command(*w,IDM_HOST_PAUSE);w->openRom(fixture(root));waitFor(*w,[](const auto& s){return s.romChanged && !s.busy;});
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());if(!hook) return false;
    const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;const auto before=dismissedDialogs;
    command(*w,IDM_SAVE_STATE);const bool saveCanceled=dismissedDialogs>before;const auto beforeOpen=dismissedDialogs;
    command(*w,IDM_LOAD_STATE);
    const bool canceled=saveCanceled && dismissedDialogs>beforeOpen && MainWindowTestAccess::snapshot(*w).operationSerial==serial && !enabled(*w,IDM_SAVE_STATE);
    if(!canceled) std::cerr<<"State dialog cancellations="<<dismissedDialogs-before<<" serial="<<MainWindowTestAccess::snapshot(*w).operationSerial<<" expected="<<serial<<" menu="<<enabled(*w,IDM_SAVE_STATE)<<'\n';
    if(!UnhookWindowsHookEx(hook) || !canceled) return false;return closeWindow(*w);
}
bool fmChoice(const std::filesystem::path& root) {
    auto w=makeWindow();command(*w,IDM_HOST_PAUSE);configure(*w,[](auto& d){d.fmEnabled=true;});configure(*w,[](auto& d){d.fmPlugin=L"acesms-fm-ymfm.dll";});configure(*w,[](auto& d){d.fmMethod=smsgg::FmInterpolation::Linear;});
    const auto path=fixture(root);w->openRom(path);waitFor(*w,[](const auto& s){return s.romChanged && !s.busy;});
    const auto& current=MainWindowTestAccess::snapshot(*w);
    if(!current.fmProfile || current.fmBackendName!="ymfm OPLL" || current.fmProfile->method!=smsgg::FmInterpolation::Linear) return false;
    configure(*w,[](auto& d){d.fmPlugin.clear();});configure(*w,[](auto& d){d.fmMethod=smsgg::FmInterpolation::Sinc;});
    if(MainWindowTestAccess::snapshot(*w).fmBackendName!="ymfm OPLL" || MainWindowTestAccess::snapshot(*w).fmProfile->method!=smsgg::FmInterpolation::Linear) return false;
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());if(!hook) return false;
    command(*w,IDM_HELP_ROM_INFO);const bool info=lastDialogText.find(L"ymfm OPLL")!=std::wstring::npos && lastDialogText.find(L"3579545/72")!=std::wstring::npos
        && lastDialogText.find(L"Linear")!=std::wstring::npos && lastDialogText.find(L"compatibility")!=std::wstring::npos;
    if(!UnhookWindowsHookEx(hook) || !info) return false;
    for(const auto method:{smsgg::FmInterpolation::Sinc,smsgg::FmInterpolation::ZohLowPass,smsgg::FmInterpolation::Linear}) {
        configure(*w,[&](auto& d){d.fmMethod=method;});const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;w->openRom(path);waitOperation(*w,serial);
        const auto& s=MainWindowTestAccess::snapshot(*w);
        if(!s.error.empty() || !s.fmProfile || s.fmProfile->method!=method || s.fmBackendName=="ymfm OPLL") return false;
    }
    return closeWindow(*w);
}
bool fmExternal(const std::filesystem::path& root) {
    const auto directory=smsgg::fmPluginDirectory(),alias=directory/L"acesms-test-&-external.dll";
    std::filesystem::copy_file(directory/L"acesms-fm-ymfm.dll",alias);
    struct RemoveAlias {std::filesystem::path path;~RemoveAlias(){std::error_code error;std::filesystem::remove(path,error);}} remove{alias};
    auto w=makeWindow();command(*w,IDM_HOST_PAUSE);configure(*w,[](auto& d){d.fmEnabled=true;});
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    settingsAction=[&](HWND dialog) {
        const auto dropdown=GetDlgItem(dialog,IDC_ROM_BACKEND);
        const auto label=alias.filename();
        const auto at=SendMessageW(dropdown,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(label.c_str()));
        if(at!=CB_ERR) SendMessageW(dropdown,CB_SETCURSEL,at,0);
        PostMessageW(dialog,WM_COMMAND,at==CB_ERR?IDCANCEL:IDOK,0);
    };
    command(*w,IDM_SETTINGS);
    settingsAction={};
    if(!UnhookWindowsHookEx(hook) || MainWindowTestAccess::defaults(*w).fmPlugin!=alias.filename()) return false;
    w->openRom(fixture(root));waitFor(*w,[](const auto& s){return s.romChanged && !s.busy;});
    const auto& s=MainWindowTestAccess::snapshot(*w);
    if(s.fmBackendName!="ymfm OPLL" || !s.operationSucceeded) return false;
    return closeWindow(*w);
}
std::filesystem::path ramFixture(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);auto bytes=smsgg::test::stage5InputRom();bytes.resize(32768);bytes[1]=0;bytes[2]=8;
    const uint8_t code[]{0x3e,8,0x32,0xfc,0xff,0x3e,0x5a,0x32,0,0xa0,0xc3,0,1};std::copy(std::begin(code),std::end(code),bytes.begin()+0x800);
    const auto path=root/L"ram.sms";writeBytes(path,bytes);return path;
}
std::filesystem::path sramPath(const std::filesystem::path& rom) {
    const auto image=std::get<smsgg::RomImage>(smsgg::RomLoader::load(rom,{smsgg::MapperKind::Sega,8192}));
    return smsgg::win32::SramStore(rom,image).path();
}
bool sramClose(const std::filesystem::path& root) {
    const auto path=ramFixture(root),save=sramPath(path);std::filesystem::remove(save);
    auto w=makeWindow();configure(*w,[](auto& d){d.options.sramBytes=8192;});w->openRom(path);waitFor(*w,[](const auto& s){return s.frames>=3;});
    if(MainWindowTestAccess::snapshot(*w).ramBytes!=8192) return false;
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());if(!hook) return false;
    command(*w,IDM_HELP_ROM_INFO);const bool info=lastDialogText.find(save.wstring())!=std::wstring::npos && lastDialogText.find(L"8192")!=std::wstring::npos;
    if(!UnhookWindowsHookEx(hook) || !info || !closeWindow(*w)) return false;
    const auto data=readBytes(save);return data.size()==8192 && data[0]==0x5a;
}
bool saveFailure(const std::filesystem::path& root,bool discard) {
    const auto path=ramFixture(root),save=sramPath(path);std::filesystem::remove(save);
    auto w=makeWindow();configure(*w,[](auto& d){d.options.sramBytes=8192;});w->openRom(path);waitFor(*w,[](const auto& s){return s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});std::filesystem::create_directory(save);
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());if(!hook) return false;
    const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial,time=MainWindowTestAccess::snapshot(*w).tstates;
    sramChoice=IDCANCEL;SendMessageW(w->handle(),WM_CLOSE,0,0);waitOperation(*w,serial);
    if(!w->handle() || MainWindowTestAccess::snapshot(*w).tstates!=time || !enabled(*w,IDM_FILE_OPEN)) {UnhookWindowsHookEx(hook);return false;}
    sramChoice=discard?IDCONTINUE:IDTRYAGAIN;if(!discard) removeOnRetry=save;
    const bool closed=closeWindow(*w);if(!UnhookWindowsHookEx(hook) || !closed) return false;
    if(discard) {
        const bool kept=std::filesystem::is_directory(save) && std::filesystem::is_empty(save);
        std::filesystem::remove(save);
        return kept;
    }
    const auto data=readBytes(save);
    return data.size()==8192 && data[0]==0x5a;
}
bool switchFailure(const std::filesystem::path& root) {
    const auto path=ramFixture(root),save=sramPath(path);std::filesystem::remove(save);
    auto w=makeWindow();configure(*w,[](auto& d){d.options.sramBytes=8192;});w->openRom(path);waitFor(*w,[](const auto& s){return s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});std::filesystem::create_directory(save);
    configure(*w,[](auto& d){d.options.mapper=smsgg::MapperKind::RomOnly;});const auto next=fixture(root);auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    dialogOwner=w->handle();const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());if(!hook) return false;
    sramChoice=IDCANCEL;w->openRom(next);waitOperation(*w,serial);
    if(MainWindowTestAccess::snapshot(*w).romPath!=path || !enabled(*w,IDM_FILE_OPEN)) {UnhookWindowsHookEx(hook);return false;}
    serial=MainWindowTestAccess::snapshot(*w).operationSerial;sramChoice=IDTRYAGAIN;removeOnRetry=save;w->openRom(next);
    configure(*w,[](auto& d){d.machine=smsgg::Machine::GameGear;}); // Changing next settings must not change the accepted retry request.
    waitFor(*w,[&](const auto& s){return !s.busy && s.operationSerial>=serial+2;});
    const auto& s=MainWindowTestAccess::snapshot(*w);const bool kept=s.romPath==next && s.machine==smsgg::Machine::Sms && s.mapper==smsgg::MapperKind::RomOnly && s.error.empty();
    if(!UnhookWindowsHookEx(hook) || !kept) return false;return closeWindow(*w);
}
bool deferredClose(const std::filesystem::path& root) {
    auto w=makeWindow();w->openRom(fixture(root));waitFor(*w,[](const auto& s){return s.frames>=3 && s.audio.playing;});
    const auto state=root/L"close.state";auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;MainWindowTestAccess::stateFile(*w,state,true);waitOperation(*w,serial);
    serial=MainWindowTestAccess::snapshot(*w).operationSerial;audio->holdStop(true);MainWindowTestAccess::stateFile(*w,state,false);
    SendMessageW(w->handle(),WM_CLOSE,0,0);const bool alive=IsWindow(w->handle()) && !enabled(*w,IDM_FILE_OPEN);
    audio->holdStop(false);const bool closed=closeWindow(*w);
    return alive && closed && MainWindowTestAccess::snapshot(*w).closeReady && MainWindowTestAccess::snapshot(*w).operationSerial==serial+2;
}
}
bool dbRegistration(const std::filesystem::path& root) {
    using namespace smsgg::win32;
    const auto plain=fixture(root);
    const auto prefixed=fixture(root,true);
    const auto dbPath=root/L"registration.sqlite3";
    std::filesystem::remove(dbPath);
    auto w=makeWindow();
    command(*w,IDM_HOST_PAUSE);
    w->openRom(plain);
    waitOperation(*w,0);
    const auto time=MainWindowTestAccess::snapshot(*w).tstates;
    MainWindowTestAccess::database(*w,dbPath);
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    registrationChoice=IDCANCEL;
    w->openRom(prefixed);
    waitOperation(*w,1);
    const auto& cancelled=MainWindowTestAccess::snapshot(*w);
    if(dismissedDialogs!=1 || !cancelled.unregisteredRom || cancelled.databaseRecord || cancelled.romPath!=plain ||
        cancelled.tstates!=time || lastDialogText.find(L"512")==std::wstring::npos) return false;
    auto opened=RomDatabase::open(dbPath);
    auto& db=std::get<RomDatabase>(opened);
    const auto identity=cancelled.unregisteredRom->identity;
    if(std::get<std::optional<RomRecord>>(db.findRom(identity))) return false;

    registrationChoice=IDOK;
    w->openRom(prefixed);
    waitFor(*w,[](const auto& s){return !s.busy && s.operationSerial>=4;});
    const auto& registered=MainWindowTestAccess::snapshot(*w);
    if(dismissedDialogs!=2 || !registered.operationSucceeded || !registered.databaseRecord || registered.ramBytes!=32768 ||
        registered.romPath!=prefixed || enabled(*w,IDM_SAVE_STATE) || enabled(*w,IDM_LOAD_STATE)) return false;
    const auto record=*registered.databaseRecord;
    settingsAction=[](HWND dialog) {
        SendDlgItemMessageW(dialog,IDC_ROM_MACHINE,CB_SETCURSEL,1,0);
        SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(IDC_ROM_MACHINE,CBN_SELCHANGE),
            reinterpret_cast<LPARAM>(GetDlgItem(dialog,IDC_ROM_MACHINE)));
        PostMessageW(dialog,WM_COMMAND,IDOK,0);
    };
    command(*w,IDM_SETTINGS);
    settingsAction={};
    if(MainWindowTestAccess::defaults(*w).machine!=smsgg::Machine::GameGear) return false;
    w->openRom(plain);
    waitOperation(*w,4);
    const auto& reused=MainWindowTestAccess::snapshot(*w);
    if(dismissedDialogs!=3 || !reused.operationSucceeded || reused.databaseRecord!=record || reused.romPath!=plain) return false;
    command(*w,IDM_HELP_ROM_INFO);
    const auto contains=[&](std::wstring_view value){return lastDialogText.find(value)!=std::wstring::npos;};
    if(!contains(L"Database: "+dbPath.wstring()) || !contains(L"ROM ID: "+std::to_wstring(record.id)) ||
        !contains(L"Last registered path: "+prefixed.wstring()) || !contains(L"SHA-256: ") || !contains(L"SHA-1: ") ||
        !contains(L"Saved settings: SMS / NTSC / Sega / 32768 bytes") ||
        !contains(L"Running profile: SMS / NTSC / 48000 Hz") || !contains(L"Sega SRAM: ROM database"))
        throw std::runtime_error("ROM information did not identify the active database and settings");
    return closeWindow(*w);
}
bool dbRomDrop(const std::filesystem::path& root) {
    const auto plain=fixture(root);
    auto w=makeWindow();
    const auto hwnd=w->handle();
    if(!(GetWindowLongW(hwnd,GWL_EXSTYLE)&WS_EX_ACCEPTFILES)) {
        std::cerr<<"Main window does not accept shell file drops\n";
        return false;
    }
    const auto drop=[&](std::initializer_list<std::filesystem::path> paths) {
        std::wstring names;
        for(const auto& path:paths) {
            names+=path.native();
            names+=L'\0';
        }
        names+=L'\0';
        const auto bytes=names.size()*sizeof(wchar_t);
        const auto handle=GlobalAlloc(GMEM_MOVEABLE,sizeof(DROPFILES)+bytes);
        if(!handle) throw std::runtime_error("Cannot allocate shell drop fixture");
        auto* data=static_cast<DROPFILES*>(GlobalLock(handle));
        if(!data) {
            GlobalFree(handle);
            throw std::runtime_error("Cannot lock shell drop fixture");
        }
        *data={sizeof(DROPFILES),{},FALSE,TRUE};
        std::memcpy(data+1,names.data(),bytes);
        GlobalUnlock(handle);
        // The receiver owns the handle after delivery.
        SendMessageW(hwnd,WM_DROPFILES,reinterpret_cast<WPARAM>(handle),0);
    };
    const auto alias=[&](const wchar_t* name) {
        const auto to=root/name;
        std::filesystem::copy_file(plain,to,std::filesystem::copy_options::overwrite_existing);
        return to;
    };
    const auto gg=alias(L"drop \u65e5\u672c\u8a9e.GG");
    const auto sg=alias(L"drop.sg");
    const auto ignored=alias(L"drop.txt");
    const auto zip=root/L"drop.ZIP";
    std::ifstream input(plain,std::ios::binary);
    const std::string payload{std::istreambuf_iterator<char>(input),{}};
    smsgg::test::writeZip(zip,{{"folder/game.SG",payload,MZ_NO_COMPRESSION}});
    command(*w,IDM_HOST_PAUSE);
    w->openRom(plain);
    waitOperation(*w,0);
    const auto dbPath=root/"drop.sqlite3";
    std::filesystem::remove(dbPath);
    MainWindowTestAccess::database(*w,dbPath);
    dialogOwner=hwnd;
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};

    drop({ignored});
    drop({gg,sg});
    if(!enabled(*w,IDM_FILE_OPEN) || std::filesystem::exists(dbPath)) return false;
    registrationChoice=IDCANCEL;
    drop({gg});
    waitOperation(*w,1);
    const auto& snapshot=MainWindowTestAccess::snapshot(*w);
    if(dismissedDialogs!=1 || !snapshot.unregisteredRom || snapshot.romPath!=plain || snapshot.databaseRecord)
        return false;
    registrationChoice=IDOK;
    drop({gg});
    drop({sg}); // The first accepted drop has marked the shared load entry busy.
    waitFor(*w,[](const auto& s) {return !s.busy && s.operationSerial==4;});
    if(dismissedDialogs!=2 || !snapshot.operationSucceeded || !snapshot.databaseRecord ||
        snapshot.romPath!=gg || snapshot.ramBytes!=32768) return false;
    const auto record=*snapshot.databaseRecord;
    for(const auto& path:{sg,zip,plain}) {
        const auto serial=snapshot.operationSerial;
        drop({path});
        waitOperation(*w,serial);
        if(dismissedDialogs!=2 || !snapshot.operationSucceeded || snapshot.romPath!=path ||
            snapshot.databaseRecord!=record) return false;
        wchar_t title[512]{};
        GetWindowTextW(hwnd,title,512);
        if((std::wstring_view(title).find(L" / folder/game.SG")!=std::wstring_view::npos)!=(path==zip))
            return false;
    }
    return closeWindow(*w);
}
bool dbFmDefaults(const std::filesystem::path& root) {
    const auto pal=fixture(root);
    const auto gg=root/"colors.gg";
    const auto ntsc=root/"ntsc.sms";
    const auto db=root/"fm-defaults.sqlite3";
    std::filesystem::remove(db);
    writeBytes(gg,smsgg::test::stage7GameGearRom());
    auto bytes=smsgg::test::stage5InputRom();
    bytes.back()^=1;
    writeBytes(ntsc,bytes);
    auto w=makeWindow(std::filesystem::absolute(db));
    MainWindowTestAccess::database(*w,db);
    command(*w,IDM_HOST_PAUSE);
    static unsigned inspected;
    static bool valid;
    inspected=0;
    valid=true;
    const auto hook=SetWindowsHookExW(WH_CBT,[](int code,WPARAM value,LPARAM data)->LRESULT {
        if(code==HCBT_ACTIVATE) {
            const auto window=reinterpret_cast<HWND>(value);
            wchar_t caption[32]{};
            GetWindowTextW(window,caption,32);
            if(std::wstring_view(caption)==L"Register ROM") {
                const auto selected=[&](int id){return SendDlgItemMessageW(window,id,CB_GETCURSEL,0,0);};
                const auto select=[&](int id,int choice) {
                    SendDlgItemMessageW(window,id,CB_SETCURSEL,choice,0);
                    SendMessageW(window,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(window,id)));
                };
                const auto fmMatches=[&](bool available) {
                    return IsDlgButtonChecked(window,IDC_ROM_FM)==BST_CHECKED &&
                        bool(IsWindowEnabled(GetDlgItem(window,IDC_ROM_FM)))==available &&
                        bool(IsWindowEnabled(GetDlgItem(window,IDC_ROM_BACKEND)))==available &&
                        bool(IsWindowEnabled(GetDlgItem(window,IDC_ROM_INTERPOLATION)))==available;
                };
                const auto current=inspected++;
                valid=valid && selected(IDC_ROM_MACHINE)==(current==0?1:0) &&
                    selected(IDC_ROM_VIDEO)==(current==1?1:0) && selected(IDC_ROM_RAM)==3 && fmMatches(current==2);
                if(current==0) {
                    select(IDC_ROM_MACHINE,0);
                    valid=valid && fmMatches(true);
                    select(IDC_ROM_VIDEO,1);
                    valid=valid && fmMatches(false);
                    select(IDC_ROM_VIDEO,0);
                    valid=valid && fmMatches(true);
                    select(IDC_ROM_MACHINE,1);
                    valid=valid && fmMatches(false);
                }
                if (current == 2) {
                    const auto mapper = static_cast<int>(selected(IDC_ROM_MAPPER));
                    select(IDC_ROM_MAPPER, -1);
                    SendMessageW(window, WM_COMMAND, IDOK, 0);
                    wchar_t error[256]{};
                    GetDlgItemTextW(window, IDC_ROM_ERROR, error, 256);
                    valid = valid && std::wstring_view(error).starts_with(L"Select a machine");
                    select(IDC_ROM_MAPPER, mapper);
                }
                PostMessageW(window,WM_COMMAND,valid?IDOK:IDCANCEL,0);
            }
        }
        return CallNextHookEx(nullptr,code,value,data);
    },nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    for(const auto& path:{gg,pal,ntsc}) {
        if(path!=gg) configure(*w,[&](auto& d){d.standard=path==pal?smsgg::VideoStandard::Pal:smsgg::VideoStandard::Ntsc;});
        const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
        w->openRom(path);
        waitFor(*w,[&](const auto& s){
            return !valid || !s.error.empty() || (!s.busy && s.operationSerial>=serial+2);
        });
        const auto& s=MainWindowTestAccess::snapshot(*w);
        if(!valid || !s.operationSucceeded || !s.databaseRecord || s.romPath!=path ||
           s.fmEnabled!=(path==ntsc) || s.databaseRecord->settings.fmEnabled!=(path==ntsc)) {
            std::cerr << "Registration checks failed (dialog " << inspected << ", valid " << valid << ") " << s.error << '\n';
            return false;
        }
    }
    return inspected==3 && closeWindow(*w);
}
bool dbSg1000(const std::filesystem::path& root) {
    using smsgg::Machine;
    namespace pad = smsgg::test::xinput;
    std::filesystem::create_directories(root);
    const auto path = root / L"diagnostic.sg";
    const auto db = root / L"sg1000.sqlite3";
    std::filesystem::remove(db);
    writeBytes(path, smsgg::test::sg1000TestRom());
    auto w = makeWindow(std::filesystem::absolute(db));
    MainWindowTestAccess::database(*w, db);
    static unsigned opened;
    static bool controlsCorrect;
    opened = 0;
    controlsCorrect = true;
    const auto hook = SetWindowsHookExW(WH_CBT, [](int code, WPARAM value, LPARAM data) -> LRESULT {
        if (code == HCBT_ACTIVATE) {
            const auto window = reinterpret_cast<HWND>(value);
            wchar_t caption[32]{};
            GetWindowTextW(window, caption, 32);
            if (std::wstring_view(caption) == L"Register ROM") {
                ++opened;
                if (opened == 2) controlsCorrect = controlsCorrect &&
                    SendDlgItemMessageW(window, IDC_ROM_MACHINE, CB_GETCURSEL, 0, 0) == static_cast<int>(Machine::Sg1000);
                const auto choice = SendDlgItemMessageW(window, IDC_ROM_MACHINE, CB_SETCURSEL,
                    static_cast<int>(Machine::Sg1000), 0);
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDC_ROM_MACHINE, CBN_SELCHANGE),
                    reinterpret_cast<LPARAM>(GetDlgItem(window, IDC_ROM_MACHINE)));
                controlsCorrect = controlsCorrect && choice == static_cast<int>(Machine::Sg1000)
                    && IsDlgButtonChecked(window, IDC_ROM_FM) == BST_CHECKED;
                for (auto id : {IDC_ROM_VIDEO, IDC_ROM_MAPPER, IDC_ROM_RAM}) {
                    controlsCorrect = controlsCorrect && !IsWindowEnabled(GetDlgItem(window, id))
                        && SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0) == 0;
                }
                for (auto id : {IDC_ROM_FM, IDC_ROM_BACKEND, IDC_ROM_INTERPOLATION})
                    controlsCorrect = controlsCorrect && !IsWindowEnabled(GetDlgItem(window, id));
                PostMessageW(window, WM_COMMAND, opened == 2 && controlsCorrect ? IDOK : IDCANCEL, 0);
            }
        }
        return CallNextHookEx(nullptr, code, value, data);
    }, nullptr, GetCurrentThreadId());
    if (!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    w->openRom(path);
    waitFor(*w, [](const auto& s) { return opened || !s.error.empty(); });
    if (!controlsCorrect || MainWindowTestAccess::snapshot(*w).databaseRecord) return false;
    configure(*w,[](auto& d){d.machine=smsgg::Machine::Sg1000;});
    w->openRom(path);
    const auto background = [](const auto& s, uint8_t red, uint8_t green, uint8_t blue) {
        const auto p = s.pixels[255];
        return p.r == red && p.g == green && p.b == blue;
    };
    waitFor(*w, [&](const auto& s) { return !s.error.empty() || (s.frames >= 2 && background(s, 33, 200, 66)); });
    const auto& s = MainWindowTestAccess::snapshot(*w);
    if (opened != 2 || !s.databaseRecord || s.machine != Machine::Sg1000 || s.fmEnabled || s.ramBytes
        || s.displayArea.width != 256 || s.displayArea.height != 192) return false;
    wchar_t title[256]{};
    GetWindowTextW(w->handle(), title, 256);
    if (std::wstring_view(title).find(L" | SG-1000 | ") == std::wstring_view::npos) return false;

    SendMessageW(w->handle(), WM_SETFOCUS, 0, 0);
    MainWindowTestAccess::useGamepadInput(*w);
    pad::results[0] = ERROR_SUCCESS;
    auto& buttons = pad::states[0].Gamepad.wButtons;
    MainWindowTestAccess::pollGamepad(*w);
    buttons = XINPUT_GAMEPAD_A;
    MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [&](const auto& live) { return background(live, 252, 85, 84); });
    buttons = XINPUT_GAMEPAD_START;
    MainWindowTestAccess::pollGamepad(*w);
    waitFor(*w, [&](const auto& live) { return background(live, 84, 85, 237); });
    const auto held = MainWindowTestAccess::snapshot(*w).frames;
    waitFor(*w, [&](const auto& live) { return live.frames >= held + 3; });
    if (!background(MainWindowTestAccess::snapshot(*w), 84, 85, 237)) return false;
    MainWindowTestAccess::useGamepadInput(*w,false);
    press(*w, VK_RETURN);
    release(*w, VK_RETURN);
    waitFor(*w, [&](const auto& live) { return background(live, 33, 200, 66); });
    return closeWindow(*w);
}

bool dbEdit(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    const auto path=fixture(root);
    const auto dbPath=root/L"edit.sqlite3";
    std::filesystem::remove(dbPath);
    auto w=makeWindow();
    MainWindowTestAccess::database(*w,dbPath);
    command(*w,IDM_HOST_PAUSE);
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    registrationChoice=IDOK;
    w->openRom(path);
    waitFor(*w,[](const auto& s){return s.operationSerial==2 && s.operationSucceeded && !s.busy;});
    if(!enabled(*w,IDM_EDIT_ROM)) return false;
    const auto record=*MainWindowTestAccess::snapshot(*w).databaseRecord;
    auto opened=RomDatabase::open(dbPath);
    auto& db=std::get<RomDatabase>(opened);
    const auto seed=[&](uint32_t ram,VideoStandard video,const wchar_t* name) {
        auto rom=std::get<RomImage>(RomLoader::load(path,{MapperKind::Sega,ram}));
        EmulationSession session(std::move(rom),path,video,48000);
        auto state=std::make_unique<EmulatorSystemState>();
        session.system().captureState(*state);
        return std::get<int64_t>(db.saveState(record.id,name,session.profile(),*state));
    };
    const auto kept=seed(16384,VideoStandard::Ntsc,L"keep me");
    const auto converted=seed(32768,VideoStandard::Ntsc,L"resize me");
    seed(32768,VideoStandard::Pal,L"delete me");
    const auto before=readBytes(dbPath);
    editChoice=IDCANCEL;
    command(*w,IDM_EDIT_ROM);
    if(dismissedDialogs!=2 || readBytes(dbPath)!=before || MainWindowTestAccess::snapshot(*w).operationSerial!=2) return false;
    editChoice=IDOK;impactChoice=IDCANCEL;
    command(*w,IDM_EDIT_ROM);
    waitOperation(*w,2);
    if(dismissedDialogs!=4 || lastDialogText.find(L"Keep (1)")==std::wstring::npos ||
        lastDialogText.find(L"Convert (1)")==std::wstring::npos || lastDialogText.find(L"Delete (1)")==std::wstring::npos ||
        lastDialogText.find(L"delete me")==std::wstring::npos || lastDialogText.find(L"16384 bytes") == std::wstring::npos ||
        MainWindowTestAccess::snapshot(*w).databaseRecord!=record || readBytes(dbPath)!=before) return false;
    impactChoice=IDOK;
    changeStatesOnReview=&db;
    command(*w,IDM_EDIT_ROM);
    waitFor(*w,[](const auto& s){return !s.busy && s.operationSerial==5;});
    const auto& refused=MainWindowTestAccess::snapshot(*w);
    if(!changedStatesOnReview || refused.operationSucceeded || refused.databaseRecord!=record ||
        refused.error.find("review the changes again")==std::string::npos ||
        std::get<std::vector<StateSummary>>(db.listStates(record.id)).size()!=3) return false;

    command(*w,IDM_EDIT_ROM);
    waitFor(*w,[](const auto& s){return !s.busy && s.operationSerial==7;});
    const auto& updated=MainWindowTestAccess::snapshot(*w);
    auto states=std::get<std::vector<StateSummary>>(db.listStates(record.id));
    const StateProfile profile{record.identity.bytes,record.identity.crc32,Machine::Sms,VideoStandard::Ntsc,MapperKind::Sega,16384,48000,{}};
    return dismissedDialogs==9 && updated.operationSucceeded && updated.ramBytes==16384 &&
        states.size()==2 && states[0].id==kept && states[1].id==converted &&
        std::holds_alternative<SavedState>(db.readState(record.id,converted,profile)) && closeWindow(*w);
}
bool aspectWriteConflict(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    MSG message{};
    while (PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
    const auto path = fixture(root);
    const auto dbPath = std::filesystem::absolute(root / "aspect-conflict.sqlite3");
    std::filesystem::remove(dbPath);
    auto opened = RomDatabase::open(dbPath);
    auto& db = std::get<RomDatabase>(opened);
    RomSettings settings;
    settings.mapper = MapperKind::RomOnly;
    const auto rom = std::get<RomImage>(RomLoader::load(path, settings.loadOptions()));
    const auto record = std::get<RomRecord>(db.registerRom(std::get<RomIdentity>(identifyRom(rom)), settings, L"Aspect", path));
    auto w = makeWindow(dbPath);
    MainWindowTestAccess::database(*w, dbPath);
    w->openRom(path);
    waitFor(*w, [](const auto& s) { return s.operationSucceeded && s.audio.playing; });
    dialogOwner = w->handle();
    const auto hook = SetWindowsHookExW(WH_CBT, dismissDialog, nullptr, GetCurrentThreadId());
    if (!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    const auto savedRatio = [&] { return std::get<AppSettings>(db.readAppSettings()).fourThree; };
    const auto shownRatio = [&] { return GetMenuState(GetMenu(w->handle()),IDM_ASPECT_4_3,MF_BYCOMMAND) & MF_CHECKED; };
    if (savedRatio() || shownRatio()) return false;

    settings.video = VideoStandard::Pal;
    const auto serial = MainWindowTestAccess::snapshot(*w).operationSerial;
    bool guarded = false;
    {
        audio->holdStop(true);
        struct ReleaseStop { ~ReleaseStop() { audio->holdStop(false); } } release;
        if (!MainWindowTestAccess::updateRom(*w,{settings,record.name,path})) return false;
        waitFor(*w, [](const auto& s) {
            const auto control = audio->stream.control();
            return s.busy && control.command == AudioCommand::Stopped && audio->stream.status().acknowledged < control.sequence;
        });
        const auto competing = RomDatabase::open(dbPath);
        const auto* busy = std::get_if<DatabaseError>(&competing);
        const auto dialogs = dismissedDialogs;
        const bool menuDisabled = !enabled(*w,IDM_ASPECT_SQUARE) && !enabled(*w,IDM_ASPECT_4_3);
        command(*w,IDM_ASPECT_4_3);
        guarded = busy && busy->kind == DatabaseErrorKind::Sqlite && busy->code == SQLITE_BUSY &&
            menuDisabled && dismissedDialogs == dialogs && !savedRatio() && !shownRatio();
    }
    waitOperation(*w, serial);
    if (!guarded) throw std::runtime_error("View aspect wrote or reported an error during a worker transaction");
    if (!MainWindowTestAccess::snapshot(*w).operationSucceeded || !enabled(*w,IDM_ASPECT_4_3)) return false;

    if (db.execute("BEGIN IMMEDIATE")) return false;
    const auto dialogs = dismissedDialogs;
    command(*w,IDM_ASPECT_4_3);
    const bool unchanged = dismissedDialogs > dialogs && lastDialogText.find(L"locked") != std::wstring::npos &&
        !shownRatio();
    if (db.execute("ROLLBACK") || !unchanged || savedRatio()) return false;
    command(*w,IDM_ASPECT_4_3);
    return shownRatio() && savedRatio() && closeWindow(*w);
}

bool about() {
    auto w=makeWindow();
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};

    const auto menu=GetMenu(w->handle());
    wchar_t label[64]{};
    GetMenuStringW(menu,IDM_HELP_ABOUT,label,64,MF_BYCOMMAND);
    command(*w,IDM_HELP_ABOUT);
    if(std::wstring_view(label)!=L"&About..." ||
        GetMenuState(menu,IDM_HELP_ROM_INFO,MF_BYCOMMAND)==UINT(-1))
        throw std::runtime_error("Help should contain About and ROM information");
    if(dismissedDialogs!=1 || lastDialogText.find(L"acesms " ACESMS_VERSION_WIDE L"\n")==std::wstring::npos ||
        lastDialogText.find(L"Copyright (c) 2026 ACoward")==std::wstring::npos ||
        lastDialogText.find(L"MIT License")==std::wstring::npos ||
        lastDialogText.find(L"https://github.com/ACoward-Kakuyomu/acesms")==std::wstring::npos)
        throw std::runtime_error("About should show product version, copyright, license and project URL");
    return closeWindow(*w);
}

bool dbSramAutosave(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    const auto first=ramFixture(root);
    const auto second=root/L"second.sms";
    auto bytes=readBytes(first);
    bytes[0x806]=0x6b;
    writeBytes(second,bytes);
    const auto dbPath=root/L"autosave.sqlite3";
    std::filesystem::remove(dbPath);
    auto w=makeWindow();
    MainWindowTestAccess::database(*w,dbPath);
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};

    registrationChoice=IDOK;
    w->openRom(first);
    waitFor(*w,[](const auto& s){return s.operationSucceeded && s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    constexpr unsigned retiredSaveSramCommand=1005;
    if(GetMenuState(GetMenu(w->handle()),retiredSaveSramCommand,MF_BYCOMMAND)!=UINT(-1))
        throw std::runtime_error("File menu still exposes manual SRAM saving");

    auto opened=RomDatabase::open(dbPath);
    auto& db=std::get<RomDatabase>(opened);
    const auto& snapshot=MainWindowTestAccess::snapshot(*w);
    const auto firstRecord=*snapshot.databaseRecord;
    DbSramStore firstSram(db,firstRecord);
    const auto before=readBytes(dbPath);
    const auto time=snapshot.tstates;
    registrationChoice=IDCANCEL;
    auto serial=snapshot.operationSerial;
    w->openRom(second);
    waitOperation(*w,serial);
    if(snapshot.romPath!=first || snapshot.tstates!=time || readBytes(dbPath)!=before ||
        std::get<std::vector<uint8_t>>(firstSram.load())[0x2000]!=0)
        throw std::runtime_error("canceled switch saved SRAM or replaced the session");

    registrationChoice=IDOK;
    w->openRom(second);
    waitFor(*w,[&](const auto& s){return !s.busy && s.romPath==second;});
    if(std::get<std::vector<uint8_t>>(firstSram.load())[0x2000]!=0x5a)
        throw std::runtime_error("switch did not save dirty SRAM");
    const auto secondRecord=*snapshot.databaseRecord;
    DbSramStore secondSram(db,secondRecord);
    if(std::get<std::vector<uint8_t>>(secondSram.load())[0x2000]!=0) return false;
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});

    if(db.execute("CREATE TRIGGER reject_sram_save BEFORE UPDATE ON sram BEGIN SELECT RAISE(ABORT,'save failed'); END")) return false;
    serial=snapshot.operationSerial;
    sramChoice=IDCANCEL;
    SendMessageW(w->handle(),WM_CLOSE,0,0);
    waitOperation(*w,serial);
    if(!w->handle() || !snapshot.sramSaveFailed || snapshot.romPath!=second ||
        std::get<std::vector<uint8_t>>(secondSram.load())[0x2000]!=0)
        throw std::runtime_error("failed close did not preserve SRAM and session");
    if(db.execute("DROP TRIGGER reject_sram_save") || !closeWindow(*w) ||
        std::get<std::vector<uint8_t>>(secondSram.load())[0x2000]!=0x6b)
        throw std::runtime_error("normal exit did not save dirty SRAM");
    w.reset();

    if(db.execute("CREATE TRIGGER reject_sram_save BEFORE UPDATE ON sram BEGIN SELECT RAISE(ABORT,'unexpected write'); END")) return false;
    MSG message{};
    while(PeekMessageW(&message,nullptr,WM_QUIT,WM_QUIT,PM_REMOVE)) {}
    w=makeWindow();
    MainWindowTestAccess::database(*w,dbPath);
    dialogOwner=w->handle();
    w->openRom(second);
    waitFor(*w,[](const auto& s){return s.operationSucceeded && s.frames>=3;});
    const auto unchanged=readBytes(dbPath);
    w->openRom(first);
    waitOperation(*w,1);
    if(MainWindowTestAccess::snapshot(*w).romPath!=first || !closeWindow(*w) || readBytes(dbPath)!=unchanged)
        throw std::runtime_error("unchanged SRAM caused a database write");
    return true;
}
bool dbSramClear(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    const auto path=ramFixture(root);
    const auto dbPath=root/L"clear.sqlite3";
    std::filesystem::remove(dbPath);
    auto w=makeWindow();
    MainWindowTestAccess::database(*w,dbPath);
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    registrationChoice=IDOK;
    w->openRom(path);
    waitFor(*w,[](const auto& s){return s.operationSucceeded && s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);command(*w,IDM_MUTE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused && s.muted;});
    w->openRom(path);
    waitOperation(*w,2);
    const auto record=*MainWindowTestAccess::snapshot(*w).databaseRecord;
    const auto time=MainWindowTestAccess::snapshot(*w).tstates;
    const auto frames=MainWindowTestAccess::snapshot(*w).frames;
    const auto sequence=audio->stream.control().sequence;
    auto opened=RomDatabase::open(dbPath);
    auto& db=std::get<RomDatabase>(opened);
    DbSramStore probe(db,record);
    const auto readRam=[&]{return std::get<std::vector<uint8_t>>(probe.load());};
    const auto original=readRam();
    if(original.size()!=32768 || original[0x2000]!=0x5a) throw std::runtime_error("SRAM fixture did not persist dirty RAM");
    auto rom=std::get<RomImage>(RomLoader::load(path,{MapperKind::Sega,32768}));
    EmulationSession session(std::move(rom),path,VideoStandard::Ntsc,48000);
    auto state=std::make_unique<EmulatorSystemState>();
    session.system().captureState(*state);
    state->cartridge.ram[0]=0x44;
    const auto id=std::get<int64_t>(db.saveState(record.id,L"past RAM",session.profile(),*state));
    const auto before=readBytes(dbPath);
    const auto unchanged=[&] {
        const auto& s=MainWindowTestAccess::snapshot(*w);
        return s.databaseRecord==record && s.tstates==time && s.frames==frames && s.state==RunnerState::Paused &&
            s.muted && audio->stream.control().sequence==sequence;
    };
    editChoice=IDC_ROM_CLEAR_SRAM;clearChoice=IDNO;
    command(*w,IDM_EDIT_ROM);
    if(currentSramDescription!=L"Current SRAM: 32 KiB" || clearDescription.find(L"32 KiB")==std::wstring::npos ||
        clearDescription.find(L"states")==std::wstring::npos) {
        std::wcerr<<L"Capacity: "<<currentSramDescription<<L" Confirmation: "<<clearDescription<<L'\n';
        throw std::runtime_error("SRAM capacity or confirmation text");
    }
    if(readBytes(dbPath)!=before || !unchanged() || MainWindowTestAccess::snapshot(*w).operationSerial!=3)
        throw std::runtime_error("cancel changed the SRAM database or session");

    if(db.execute("CREATE TRIGGER reject_clear BEFORE UPDATE ON sram BEGIN SELECT RAISE(ABORT,'clear failed'); END")) return false;
    editChoice=IDC_ROM_CLEAR_SRAM;clearChoice=IDYES;
    command(*w,IDM_EDIT_ROM);waitOperation(*w,3);
    if(MainWindowTestAccess::snapshot(*w).operationSucceeded || readRam()!=original || !unchanged() ||
        db.execute("DROP TRIGGER reject_clear")) throw std::runtime_error("failed clear changed database or session");
    press(*w,'2');release(*w,'2');press(*w,VK_F5);release(*w,VK_F5);
    waitOperation(*w,4);
    const auto checkpointId=MainWindowTestAccess::snapshot(*w).savedStateId;
    if(!checkpointId) return false;
    const auto checkpoint=db.readState(record.id,*checkpointId,session.profile());
    const auto* captured=std::get_if<SavedState>(&checkpoint);
    if(!captured || captured->state->cartridge.ram!=original)
        throw std::runtime_error("failed clear changed running RAM");

    editChoice=IDC_ROM_CLEAR_SRAM;
    command(*w,IDM_EDIT_ROM);waitOperation(*w,5);
    const std::vector<uint8_t> zero(32768);
    if(!MainWindowTestAccess::snapshot(*w).operationSucceeded || readRam()!=zero || !unchanged())
        throw std::runtime_error("approved clear did not zero SRAM or changed session");
    const auto stored=db.readState(record.id,id,session.profile());
    const auto* saved=std::get_if<SavedState>(&stored);
    return saved && saved->name==L"past RAM" &&
        encodeState(session.profile(),*saved->state)==encodeState(session.profile(),*state) && closeWindow(*w) && readRam()==zero;
}
bool fps(const std::filesystem::path& root) {
    auto w=makeWindow();
    const auto text=[&]() -> const auto& {return MainWindowTestAccess::fpsText(*w);};
    const auto positive=[&](const auto&) {return text().starts_with("FPS ") && std::stod(text().substr(4))>0;};
    const auto toggle=[&] {press(*w,VK_F7);release(*w,VK_F7);};
    w->openRom(fixture(root));
    waitFor(*w,[](const auto& s){return s.frames>=3;});
    if(!text().empty()) return false;
    toggle();
    if(!text().empty()) return false;
    waitFor(*w,positive);
    press(*w,VK_F7,true);release(*w,VK_F7);
    if(text().empty()) return false;

    const auto stateFile=root/"fps.state";
    auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    MainWindowTestAccess::stateFile(*w,stateFile,true);
    waitOperation(*w,serial);
    if(!MainWindowTestAccess::snapshot(*w).operationSucceeded || text().empty()) return false;
    command(*w,IDM_FAST_FORWARD);
    waitFor(*w,[](const auto& s){return s.fastForward;});
    if(!text().empty()) return false;
    waitFor(*w,positive);
    serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    MainWindowTestAccess::stateFile(*w,stateFile,false);
    waitOperation(*w,serial);
    if(!MainWindowTestAccess::snapshot(*w).operationSucceeded || !text().empty()) return false;
    waitFor(*w,positive);
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    if(text()!="FPS 0.0") return false;
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Running;});
    if(!text().empty()) return false;
    waitFor(*w,positive);
    toggle();
    return text().empty() && closeWindow(*w);
}
bool fastForward(const std::filesystem::path& root) {
    auto w=makeWindow();
    const auto menu=GetMenu(w->handle());
    auto checked=[&] {return GetMenuState(menu,IDM_FAST_FORWARD,MF_BYCOMMAND);};
    if(checked()==UINT(-1) || (checked()&MF_CHECKED)) return false;
    press(*w,VK_F2);
    release(*w,VK_F2);
    waitFor(*w,[](const auto& s){return s.fastForward && s.state==RunnerState::Idle;});
    if(!(checked()&MF_CHECKED)) return false;
    w->openRom(fixture(root));
    waitFor(*w,[](const auto& s){return s.fastForward && s.frames>=3;});
    const auto time=MainWindowTestAccess::snapshot(*w).tstates;
    command(*w,IDM_MUTE);
    waitFor(*w,[&](const auto& s){return s.fastForward && s.muted && s.tstates>time;});
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    command(*w,IDM_FAST_FORWARD);
    waitFor(*w,[](const auto& s){return !s.fastForward;});
    const auto pausedTime=MainWindowTestAccess::snapshot(*w).tstates;
    if(checked()&MF_CHECKED) return false;
    const auto path=root/"paused.state";
    auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    MainWindowTestAccess::stateFile(*w,path,true);
    waitOperation(*w,serial);
    if(!MainWindowTestAccess::snapshot(*w).operationSucceeded) return false;
    command(*w,IDM_FAST_FORWARD);
    waitFor(*w,[](const auto& s){return s.fastForward;});
    serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    MainWindowTestAccess::stateFile(*w,path,false);
    waitOperation(*w,serial);
    const auto& restored=MainWindowTestAccess::snapshot(*w);
    if(!restored.operationSucceeded || !restored.fastForward || restored.state!=RunnerState::Paused ||
       restored.tstates!=pausedTime || !(checked()&MF_CHECKED)) return false;
    press(*w,VK_F2);
    press(*w,VK_F2,true);
    release(*w,VK_F2);
    waitFor(*w,[](const auto& s){return !s.fastForward;});
    return !(checked()&MF_CHECKED) && closeWindow(*w);
}
bool dbStates(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    const auto path=fixture(root);
    const auto dbPath=root/L"states.sqlite3";
    std::filesystem::remove(dbPath);
    auto w=makeWindow();
    if(enabled(*w,IDM_MANAGE_STATES)) return false;
    MainWindowTestAccess::database(*w,dbPath);
    command(*w,IDM_HOST_PAUSE);
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    if(!hook) return false;
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    registrationChoice=IDOK;
    w->openRom(path);
    waitFor(*w,[](const auto& s){return !s.busy && s.operationSerial==2 && s.operationSucceeded;});
    if(!enabled(*w,IDM_MANAGE_STATES)) return false;
    const auto record=*MainWindowTestAccess::snapshot(*w).databaseRecord;
    const auto operate=[&](int action,int row,int confirm,bool accepted) {
        const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
        stateAction=action;stateRow=row;stateConfirm=confirm;
        command(*w,IDM_MANAGE_STATES);
        waitFor(*w,[&](const auto& s){return !s.busy && s.operationSerial>=serial+(accepted?2:1);});
    };
    const auto quick=[&](unsigned key) {
        const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
        press(*w,key);release(*w,key);
        waitFor(*w,[&](const auto& s){return !s.busy && s.operationSerial==serial+1;});
        return MainWindowTestAccess::snapshot(*w).operationSucceeded;
    };
    press(*w,'5');release(*w,'5');
    if(MainWindowTestAccess::slotNotice(*w)!=L"Slot 5 \u2014 (Empty)") return false;
    if(!quick(VK_F5)) return false;
    const auto first=*MainWindowTestAccess::snapshot(*w).savedStateId;
    auto opened=RomDatabase::open(dbPath);
    auto& db=std::get<RomDatabase>(opened);
    const auto rows=[&]{return std::get<std::vector<StateSummary>>(db.listStates(record.id));};
    if(rows().size()!=1 || rows()[0].slot!=5) return false;
    if(!(GetMenuState(GetMenu(w->handle()),IDM_STATE_SLOT_BASE+4,MF_BYCOMMAND)&MF_CHECKED)) return false;
    const auto serialBeforeRepeat=MainWindowTestAccess::snapshot(*w).operationSerial;
    press(*w,VK_F5,true);release(*w,VK_F5);w->pumpOnce(20);
    if(MainWindowTestAccess::snapshot(*w).operationSerial!=serialBeforeRepeat) return false;
    operate(IDC_STATE_OVERWRITE,4,IDNO,true);
    if(displayedStates!=10 || rows()[0].name!=L"same name") return false;
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.frames>=3;});
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    if(!quick(VK_F6) || MainWindowTestAccess::snapshot(*w).tstates!=0) return false;
    if(!quick(VK_F5) || rows().size()!=1 || rows()[0].id!=first || rows()[0].name!=L"same name") return false;
    command(*w,IDM_STATE_SLOT_BASE+5);
    command(*w,IDM_STATE_SLOT_BASE+4);
    if(!MainWindowTestAccess::slotNotice(*w).starts_with(L"Slot 5 \u2014 ") ||
        !MainWindowTestAccess::slotNotice(*w).ends_with(L" same name")) return false;
    MainWindowTestAccess::expireSlotNotice(*w);
    if(!MainWindowTestAccess::slotNotice(*w).empty()) return false;
    press(*w,'0');release(*w,'0');
    if(!quick(VK_F5) || rows().size()!=2 || rows()[1].slot!=10) return false;
    operate(IDC_STATE_DELETE,4,IDYES,true);
    if(rows().size()!=1 || rows()[0].slot!=10) return false;
    MainWindowTestAccess::useGamepadInput(*w);
    SendMessageW(w->handle(),WM_SETFOCUS,0,0);
    namespace api=smsgg::test::xinput;
    api::results[0]=ERROR_SUCCESS;
    auto& pad=api::states[0].Gamepad;
    const auto poll=[&] {MainWindowTestAccess::pollGamepad(*w);};
    poll();
    command(*w,IDM_STATE_SLOT_BASE);
    pad.bLeftTrigger=255;poll();
    if(!MainWindowTestAccess::slotNotice(*w).starts_with(L"Slot 10 \u2014 ")) return false;
    pad.bLeftTrigger=0;poll();
    pad.bRightTrigger=255;poll();poll();
    if(MainWindowTestAccess::slotNotice(*w)!=L"Slot 1 \u2014 (Empty)") return false;
    pad.bRightTrigger=0;poll();
    pad.wButtons=XINPUT_GAMEPAD_RIGHT_THUMB;poll();
    if(GetMenu(w->handle())) return false;
    pad.wButtons=0;poll();
    pad.wButtons=XINPUT_GAMEPAD_RIGHT_THUMB;poll();
    if(!GetMenu(w->handle())) return false;
    pad.wButtons=0;poll();
    const auto quickPad=[&](WORD button,RunnerOperation operation) {
        const auto previous=MainWindowTestAccess::snapshot(*w).operationSerial;
        pad.wButtons=button;poll();
        pad.wButtons=0;poll();
        waitFor(*w,[&](const auto& s){return !s.busy && s.operationSerial>previous;});
        const auto& result=MainWindowTestAccess::snapshot(*w);
        return result.operation==operation && result.operationSucceeded;
    };
    if(!quickPad(XINPUT_GAMEPAD_LEFT_SHOULDER,RunnerOperation::SaveState) || rows()[0].slot!=1 ||
        !quickPad(XINPUT_GAMEPAD_RIGHT_SHOULDER,RunnerOperation::LoadState)) return false;
    poll(); // Restoring clears input and requires a released sample before the next shortcut.
    pad.wButtons=XINPUT_GAMEPAD_LEFT_THUMB;poll();
    waitFor(*w,[](const auto& s){return s.fastForward;});
    pad.wButtons=0;poll();
    const auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    press(*w,'5');release(*w,'5');press(*w,VK_F6);release(*w,VK_F6);
    w->pumpOnce(20);
    return MainWindowTestAccess::snapshot(*w).operationSerial==serial && closeWindow(*w);
}
bool cheatWindow(const std::filesystem::path& root) {
    using namespace smsgg;
    using namespace smsgg::win32;
    const auto rom=fixture(root), path=root/L"cheats.sqlite3";
    std::filesystem::remove(path);
    auto opened=RomDatabase::open(path);
    auto& db=std::get<RomDatabase>(opened);
    RomSettings settings;settings.mapper=MapperKind::RomOnly;
    const auto image=std::get<RomImage>(RomLoader::load(rom,settings.loadOptions()));
    const auto record=std::get<RomRecord>(db.registerRom(std::get<RomIdentity>(identifyRom(image)),settings,L"cheats",rom));
    auto w=makeWindow();MainWindowTestAccess::database(*w,path);
    const auto require=[](bool condition,const char* reason){if(!condition) throw std::runtime_error(reason);};
    ShowWindow(w->handle(),SW_SHOW);
    require(!assignablePadKey(VK_F9) && !enabled(*w,IDM_MEMORY_SEARCH),"F9 must be reserved and menu disabled at startup");
    w->openRom(rom);
    waitFor(*w,[](const auto& s){return s.operationSucceeded && s.state==RunnerState::Running;});
    const auto find=[&] {
        const auto popup=GetWindow(w->handle(),GW_ENABLEDPOPUP);
        return popup && GetDlgItem(popup,IDC_CHEAT_TABS)?popup:nullptr;
    };
    const auto awaitDialog=[&] {
        waitFor(*w,[&](const auto& s){return s.state==RunnerState::Paused && find();});
        return find();
    };
    const auto click=[](HWND dialog,unsigned id){SendMessageW(dialog,WM_COMMAND,id,0);};
    press(*w,VK_F9,true);release(*w,VK_F9);
    require(!find(),"F9 key repeat opened the dialog");
    press(*w,VK_F9);release(*w,VK_F9);
    auto dialog=awaitDialog();
    require(TabCtrl_GetCurSel(GetDlgItem(dialog,IDC_CHEAT_TABS))==0 &&
        ListView_GetItemCount(GetDlgItem(dialog,IDC_SEARCH_RESULTS))==8192,"F9 must show all SMS search results");
    SendDlgItemMessageW(dialog, IDC_SEARCH_WIDTH, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    click(dialog, IDC_SEARCH_NEW);
    wchar_t selectionError[128]{};
    GetDlgItemTextW(dialog, IDC_CHEAT_STATUS, selectionError, 128);
    require(std::wstring_view(selectionError) == L"Select a search width." &&
        ListView_GetItemCount(GetDlgItem(dialog,IDC_SEARCH_RESULTS))==8192, "unselected width changed search");
    click(dialog,IDCANCEL);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Running;});
    command(*w,IDM_HOST_PAUSE);
    waitFor(*w,[](const auto& s){return s.state==RunnerState::Paused;});
    SendMessageW(w->handle(),WM_SYSKEYDOWN,VK_F9,LPARAM{1}<<29);
    dialog=awaitDialog();
    require(TabCtrl_GetCurSel(GetDlgItem(dialog,IDC_CHEAT_TABS))==1,"Alt+F9 must select the editor");
    SetDlgItemTextW(dialog,IDC_CHEAT_NAME,L"Test");
    SetDlgItemTextW(dialog,IDC_CHEAT_ADDRESS,L"0xC090");
    SetDlgItemTextW(dialog,IDC_CHEAT_VALUE,L"90");
    CheckDlgButton(dialog,IDC_CHEAT_ENABLED,BST_CHECKED);
    SendDlgItemMessageW(dialog, IDC_CHEAT_WIDTH, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    click(dialog, IDC_CHEAT_ADD);
    GetDlgItemTextW(dialog, IDC_CHEAT_STATUS, selectionError, 128);
    require(std::wstring_view(selectionError) == L"Select a cheat width." &&
        SendDlgItemMessageW(dialog, IDC_CHEAT_LIST, LB_GETCOUNT, 0, 0)==0, "unselected width added a cheat");
    SendDlgItemMessageW(dialog, IDC_CHEAT_WIDTH, CB_SETCURSEL, 0, 0);
    click(dialog,IDC_CHEAT_ADD);
    auto serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    click(dialog,IDC_CHEAT_APPLY);waitOperation(*w,serial);
    const auto& applied=MainWindowTestAccess::snapshot(*w);
    require(applied.cheatSnapshot && applied.cheatSnapshot->ram[0x90]==90,"Apply did not update RAM");
    serial=applied.operationSerial;
    click(dialog,IDC_CHEAT_SAVE);waitOperation(*w,serial);
    require(MainWindowTestAccess::snapshot(*w).operationSucceeded &&
        std::get<std::vector<Cheat>>(db.readCheats(record.id,Machine::Sms)).size()==1,"save did not persist definitions");
    dialogOwner=w->handle();
    const auto hook=SetWindowsHookExW(WH_CBT,dismissDialog,nullptr,GetCurrentThreadId());
    require(hook!=nullptr,"unsaved dialog hook");
    struct Unhook { HHOOK handle; ~Unhook() { UnhookWindowsHookEx(handle); } } unhook{hook};
    const auto f9=[&] {PostMessageW(dialog,WM_KEYDOWN,VK_F9,0);w->pumpOnce(20);};
    SetDlgItemTextW(dialog,IDC_CHEAT_NAME,L"Discard this edit");
    cheatCloseChoice=IDCANCEL;
    f9();
    require(find()==dialog,"cancel must keep editor open");
    cheatCloseChoice=IDNO;
    f9();
    require(!find() && MainWindowTestAccess::snapshot(*w).state==RunnerState::Paused &&
        std::get<std::vector<Cheat>>(db.readCheats(record.id,Machine::Sms)).front().name==L"Test","discard or original pause failed");

    command(*w,IDM_MEMORY_SEARCH);
    dialog=awaitDialog();
    SetDlgItemTextW(dialog,IDC_SEARCH_VALUE,L"90");
    click(dialog,IDC_SEARCH_FILTER);
    const auto matches=GetDlgItem(dialog,IDC_SEARCH_RESULTS);
    require(ListView_GetItemCount(matches)>=1,"search did not see the applied value");
    NMITEMACTIVATE selected{{matches,IDC_SEARCH_RESULTS,NM_DBLCLK},0};
    SendMessageW(dialog,WM_NOTIFY,IDC_SEARCH_RESULTS,reinterpret_cast<LPARAM>(&selected));
    require(TabCtrl_GetCurSel(GetDlgItem(dialog,IDC_CHEAT_TABS))==1 &&
        IsDlgButtonChecked(dialog,IDC_CHEAT_ENABLED)==BST_UNCHECKED &&
        IsDlgButtonChecked(dialog,IDC_CHEAT_ALWAYS)==BST_UNCHECKED,"result did not create a disabled single-use draft");
    cheatCloseChoice=IDYES;
    serial=MainWindowTestAccess::snapshot(*w).operationSerial;
    f9();waitOperation(*w,serial);
    require(!find() && std::get<std::vector<Cheat>>(db.readCheats(record.id,Machine::Sms)).size()==2,
        "save-and-close did not commit the draft");
    w->pumpOnce(20);
    require(!find() && MainWindowTestAccess::snapshot(*w).state==RunnerState::Paused,"closing changed original host pause");
    return closeWindow(*w);
}

int main(int argc, char** argv) {
    if (argc != 3 || !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return 1;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    bool pass = false;
    try {
        smsgg::win32::WtlApplication application(GetModuleHandleW(nullptr));
        const std::string_view name = argv[1];
        pass = name == "controls" ? controls(argv[2]) : name == "options" ? options(argv[2])
            : name == "about" ? about() : name == "errors" ? errors(argv[2]) : name == "system_keys" ? systemKeys(argv[2])
            : name == "lifetime" ? lifetime(argv[2]) : name == "audio_failure" ? audioFailure(argv[2])
            : name == "gg" ? gg(argv[2]) : name == "fm" ? fmOptions(argv[2])
            : name == "selection" ? selectionOptions(argv[2]) : name == "xinput" ? xinput(argv[2])
            : name == "persistence" ? persistence(argv[2]) : name == "state_dialogs" ? stateDialogs(argv[2]) : name == "fm_choice" ? fmChoice(argv[2])
            : name == "fm_external" ? fmExternal(argv[2]) : name == "sram_close" ? sramClose(argv[2]) : name == "save_failure" ? saveFailure(argv[2],false) : name == "discard_close" ? saveFailure(argv[2],true)
            : name == "switch_failure" ? switchFailure(argv[2]) : name == "db_registration" ? dbRegistration(argv[2])
            : name == "db_rom_drop" ? dbRomDrop(argv[2])
            : name == "db_edit" ? dbEdit(argv[2]) : name == "db_sram_clear" ? dbSramClear(argv[2])
            : name == "db_sram_autosave" ? dbSramAutosave(argv[2])
            : name == "fast_forward" ? fastForward(argv[2])
            : name == "fullscreen" ? fullscreen()
            : name == "aspect" ? aspect(argv[2]) && aspectWriteConflict(argv[2])
            : name == "window_scale" ? windowScale(argv[2])
            : name == "fps" ? fps(argv[2])
            : name == "new_rom_defaults" ? settingsRecovery(std::filesystem::path(argv[2]) / "recovery") && newRomDefaults(argv[2])
            : name == "controllers" ? controllerSettings(argv[2])
            : name == "db_fm_defaults" ? dbFmDefaults(argv[2])
            : name == "db_sg1000" ? dbSg1000(argv[2])
            : name == "db_states" ? dbStates(argv[2]) : name == "cheats" ? cheatWindow(argv[2])
            : name == "deferred_close" && deferredClose(argv[2]);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    CoUninitialize();
    if (!pass) std::cerr << "MainWindow test failed\n";
    return pass ? 0 : 1;
}
