#include "win32/MainWindow.h"
#include "win32/CheatDialog.h"
#include "win32/Resource.h"
#include "win32/FmPlugin.h"
#include "win32/ZipRom.h"
#include "VersionResource.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdexcept>
#include <system_error>
#include <sstream>
#include <iomanip>
#include <string_view>
#include <algorithm>
#include <bit>
#include <ctime>
#include <cwchar>
#include <format>
#include <type_traits>

namespace smsgg::win32 {
namespace {
constexpr unsigned autoMachineIndex=3;
constexpr wchar_t bundledFmPlugin[] = L"acesms-fm-ymfm.dll";
constexpr uint32_t ramSizes[]{0, 8192, 16384, 32768};
constexpr wchar_t missingFmBackend[] = L"FM DLL not found. Select another backend.";

std::optional<size_t> comboChoice(HWND dialog, int id, size_t count) {
    const auto index = ::SendDlgItemMessageW(dialog, id, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<size_t>(index) >= count) return {};
    return static_cast<size_t>(index);
}

bool fmBackendAvailable(const std::filesystem::path& plugin) {
    std::error_code error;
    return plugin.empty() || std::filesystem::is_regular_file(fmPluginDirectory() / plugin, error);
}

bool canUseFm(std::optional<Machine> machine, VideoStandard video) {
    return (!machine || *machine==Machine::Sms) && video==VideoStandard::Ntsc;
}
size_t compatibleRamCapacity(MapperKind mapper, size_t bytes) {
    return mapper==MapperKind::RomOnly || (mapper==MapperKind::Codemasters && bytes>8192) ? 0 : bytes;
}

const wchar_t* mapperName(MapperKind mapper) {
    constexpr const wchar_t* names[]{L"ROM Only",L"Sega",L"Codemasters",L"Auto"};
    return names[static_cast<size_t>(mapper)];
}
const wchar_t* machineName(Machine machine) {
    if (machine == Machine::Sg1000) return L"SG-1000";
    return machine == Machine::GameGear ? L"GG" : L"SMS";
}
const wchar_t* interpolationName(FmInterpolation method) {
    constexpr const wchar_t* names[]{L"Sinc",L"Zero-order hold + low-pass filter",L"Linear"};
    return names[static_cast<size_t>(method)];
}
std::wstring wide(std::string_view text) {
    if(text.empty()) return {};
    const auto size=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!size) throw std::system_error(GetLastError(),std::system_category(),"Decode UTF-8");
    std::wstring out(size,L'\0');
    if(!MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),size)) throw std::system_error(GetLastError(),std::system_category(),"Decode UTF-8");
    return out;
}
void requireWin32(BOOL result, const char* operation) {
    if (!result) throw std::system_error(GetLastError(), std::system_category(), operation);
}
void requireHr(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed (HRESULT "
        + std::to_string(static_cast<unsigned long>(result)) + ')');
}
class RegisterRomDialog final : public ATL::CDialogImpl<RegisterRomDialog> {
public:
    enum { IDD=IDD_REGISTER_ROM };
    RegisterRomDialog(const LoadRequest& request,const UnregisteredRom& rom,
        const std::vector<std::filesystem::path>& plugins,const RomRecord* editing=nullptr)
        : request_(request),rom_(rom),backends_{std::filesystem::path{},L"acesms-fm-ymfm.dll"},editing_(editing) {
        backends_.insert(backends_.end(),plugins.begin(),plugins.end());
        if(editing_ && std::ranges::find(backends_,request.fmPlugin)==backends_.end())
            backends_.push_back(request.fmPlugin);
    }
    RomRegistration registration;
    BEGIN_MSG_MAP(RegisterRomDialog)
        MESSAGE_HANDLER(WM_INITDIALOG,initialize)
        COMMAND_HANDLER(IDC_ROM_MACHINE,CBN_SELCHANGE,profileChanged)
        COMMAND_HANDLER(IDC_ROM_VIDEO,CBN_SELCHANGE,profileChanged)
        COMMAND_ID_HANDLER(IDOK,accept)
        COMMAND_ID_HANDLER(IDCANCEL,cancel)
        COMMAND_ID_HANDLER(IDC_ROM_CLEAR_SRAM,clearSram)
    END_MSG_MAP()
private:
    void updateProfileControls() {
        const auto machineChoice = comboChoice(m_hWnd, IDC_ROM_MACHINE, 3);
        if (!machineChoice) return;
        const auto machine = static_cast<Machine>(*machineChoice);
        const bool sg1000 = machine == Machine::Sg1000;
        for (const auto id : {IDC_ROM_VIDEO, IDC_ROM_MAPPER, IDC_ROM_RAM}) {
            if (sg1000) SendDlgItemMessage(id, CB_SETCURSEL, 0, 0);
            ::EnableWindow(GetDlgItem(id), !sg1000);
        }
        const auto video = comboChoice(m_hWnd, IDC_ROM_VIDEO, 2);
        for (const auto id : {IDC_ROM_FM,IDC_ROM_BACKEND,IDC_ROM_INTERPOLATION})
            ::EnableWindow(GetDlgItem(id),video && canUseFm(machine,static_cast<VideoStandard>(*video)));
    }
    LRESULT profileChanged(WORD,WORD,HWND,BOOL&) {
        updateProfileControls();
        return 0;
    }
    uint32_t currentSramBytes() const {
        return editing_ && editing_->settings.mapper==MapperKind::Sega?editing_->settings.ramBytes:0;
    }
    LRESULT initialize(UINT,WPARAM,LPARAM,BOOL&) {
        const auto& metadata=rom_.metadata;
        const auto model=metadata.header?metadata.header->model:RomModel::Unknown;
        const wchar_t* mapperReason=L"Sega fallback";
        auto suggestedMapper=MapperKind::Sega;
        if(metadata.mapperSelection==MapperSelection::CodemastersHeader) {
            suggestedMapper=MapperKind::Codemasters;
            mapperReason=L"Codemasters checksum matched";
        } else if(metadata.mapperSelection==MapperSelection::Linear48K) {
            suggestedMapper=MapperKind::RomOnly;
            mapperReason=L"48 KiB linear ROM";
        } else if(metadata.size==40960) {
            suggestedMapper=MapperKind::RomOnly;
            mapperReason=L"40 KiB linear ROM";
        }
        std::wostringstream info;
        info<<metadata.size<<L" bytes";
        if(!editing_) info<<L" | Copier header: "<<(rom_.copierHeader==CopierHeader::Strip512?L"strip 512 bytes":L"none");
        info<<L"\nSega header: "<<(model==RomModel::Sms?L"SMS":model==RomModel::GameGear?L"Game Gear":metadata.header?L"unknown":L"absent")
            <<L" | Mapper suggestion: "<<mapperName(suggestedMapper)<<L" ("<<mapperReason<<L')';
        info<<L"\nCRC32: "<<std::hex<<std::setw(8)<<std::setfill(L'0')<<rom_.identity.crc32;
        const auto hash=[&](const wchar_t* label,const auto& bytes) {
            info<<L'\n'<<label;
            for (auto byte:bytes) info<<std::setw(2)<<unsigned(byte);
        };
        hash(L"SHA-1: ",rom_.identity.sha1);
        hash(L"SHA-256: ",rom_.identity.sha256);
        SetDlgItemText(IDC_ROM_INFO,info.str().c_str());
        if(editing_) {
            SetWindowText(L"Edit ROM settings");
            SetDlgItemText(IDOK,L"Review changes");
            SetDlgItemText(IDC_ROM_NAME,editing_->name.c_str());
            const auto bytes=currentSramBytes();
            const auto capacity=bytes?L"Current SRAM: "+std::to_wstring(bytes/1024)+L" KiB":L"Current SRAM: None";
            SetDlgItemText(IDC_ROM_SRAM_CAPACITY,capacity.c_str());
            ::EnableWindow(GetDlgItem(IDC_ROM_CLEAR_SRAM),bytes!=0);
        } else {
            const auto& name=rom_.archiveEntry.empty()?request_.path:rom_.archiveEntry;
            SetDlgItemText(IDC_ROM_NAME,name.stem().c_str());
        }
        if(!editing_) ::ShowWindow(GetDlgItem(IDC_ROM_CLEAR_SRAM),SW_HIDE);
        const auto choices=[&](int id,std::initializer_list<const wchar_t*> labels,int selection) {
            for(auto label:labels) SendDlgItemMessage(id,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            SendDlgItemMessage(id,CB_SETCURSEL,selection,0);
        };
        const auto machine=request_.machine.value_or(model==RomModel::GameGear?Machine::GameGear:Machine::Sms);
        choices(IDC_ROM_MACHINE,{L"SMS",L"Game Gear",L"SG-1000"},static_cast<int>(machine));
        const auto video=machine==Machine::Sms?request_.standard:VideoStandard::Ntsc;
        choices(IDC_ROM_VIDEO,{L"NTSC",L"PAL"},static_cast<int>(video));
        const auto mapper=request_.options.mapper==MapperKind::Auto? suggestedMapper:request_.options.mapper;
        choices(IDC_ROM_MAPPER,{L"ROM Only",L"Sega",L"Codemasters"},static_cast<int>(mapper));
        const auto ram=std::ranges::find(ramSizes,compatibleRamCapacity(mapper,request_.options.sramBytes));
        choices(IDC_ROM_RAM,{L"None",L"8 KiB",L"16 KiB",L"32 KiB"},(editing_ || request_.options.sramBytes) && ram!=std::end(ramSizes)?
            static_cast<int>(ram-std::begin(ramSizes)):-1);
        CheckDlgButton(IDC_ROM_FM,request_.fmEnabled?BST_CHECKED:BST_UNCHECKED);
        for(const auto& backend:backends_)
            SendDlgItemMessage(IDC_ROM_BACKEND,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(backend.empty()?L"Built-in emu2413":backend.c_str()));
        const auto backend=std::ranges::find(backends_,request_.fmPlugin);
        if (backend != backends_.end() && fmBackendAvailable(*backend))
            SendDlgItemMessage(IDC_ROM_BACKEND,CB_SETCURSEL,backend-backends_.begin(),0);
        else SetDlgItemText(IDC_ROM_ERROR, missingFmBackend);
        choices(IDC_ROM_INTERPOLATION,{interpolationName(FmInterpolation::Sinc),interpolationName(FmInterpolation::ZohLowPass),
            interpolationName(FmInterpolation::Linear)},static_cast<int>(request_.fmMethod));
        updateProfileControls();
        return TRUE;
    }
    LRESULT accept(WORD,WORD,HWND,BOOL&) {
        const auto ram = comboChoice(m_hWnd, IDC_ROM_RAM, std::size(ramSizes));
        if (!ram) {
            SetDlgItemText(IDC_ROM_ERROR,L"Select a cartridge RAM capacity, including None for ROMs without RAM.");
            ::SetFocus(GetDlgItem(IDC_ROM_RAM));
            return 0;
        }
        const auto machineChoice = comboChoice(m_hWnd, IDC_ROM_MACHINE, 3);
        const auto videoChoice = comboChoice(m_hWnd, IDC_ROM_VIDEO, 2);
        const auto mapper = comboChoice(m_hWnd, IDC_ROM_MAPPER, 3);
        const auto interpolation = comboChoice(m_hWnd, IDC_ROM_INTERPOLATION, 3);
        if (!machineChoice || !videoChoice || !mapper || !interpolation) {
            SetDlgItemText(IDC_ROM_ERROR, L"Select a machine, video standard, mapper and FM interpolation.");
            return 0;
        }
        const auto machine = static_cast<Machine>(*machineChoice);
        const auto video = static_cast<VideoStandard>(*videoChoice);
        const bool fmEnabled = canUseFm(machine, video) && IsDlgButtonChecked(IDC_ROM_FM) == BST_CHECKED;
        const auto backend = comboChoice(m_hWnd, IDC_ROM_BACKEND, backends_.size());
        const bool available = backend && fmBackendAvailable(backends_[*backend]);
        if (fmEnabled && !available) {
            SetDlgItemText(IDC_ROM_ERROR, missingFmBackend);
            ::SetFocus(GetDlgItem(IDC_ROM_BACKEND));
            return 0;
        }
        RomSettings settings{machine,video,static_cast<MapperKind>(*mapper),ramSizes[*ram],fmEnabled,
            available ? backends_[*backend] : request_.fmPlugin,static_cast<FmInterpolation>(*interpolation)};
        const auto nameControl=GetDlgItem(IDC_ROM_NAME);
        std::wstring name(::GetWindowTextLengthW(nameControl)+1,L'\0');
        name.resize(::GetWindowTextW(nameControl,name.data(),static_cast<int>(name.size())));
        if(auto error=validateRomRegistration(rom_.identity,settings,name,request_.path)) {
            SetDlgItemText(IDC_ROM_ERROR,wide(error->message).c_str());
            return 0;
        }
        registration={rom_.identity,std::move(settings),std::move(name)};
        EndDialog(IDOK);
        return 0;
    }
    LRESULT cancel(WORD,WORD,HWND,BOOL&) { EndDialog(IDCANCEL);return 0; }
    LRESULT clearSram(WORD,WORD,HWND,BOOL&) {
        const auto bytes=currentSramBytes();
        if(!bytes) return 0;
        const auto message=L"Clear all "+std::to_wstring(bytes/1024)+L" KiB of current SRAM?\n\n"
            L"RAM in saved states will be kept. Pending settings edits will be discarded if you clear SRAM.";
        if(::MessageBoxW(m_hWnd,message.c_str(),L"Clear SRAM",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)==IDYES)
            EndDialog(IDC_ROM_CLEAR_SRAM);
        return 0;
    }
    const LoadRequest& request_;
    const UnregisteredRom& rom_;
    std::vector<std::filesystem::path> backends_;
    const RomRecord* editing_;
};
class RomImpactDialog final : public ATL::CDialogImpl<RomImpactDialog> {
public:
    enum { IDD=IDD_ROM_IMPACT };
    RomImpactDialog(const RomUpdateImpact& impact,const RomUpdateRequest& request) {
        std::wostringstream text;
        if(request.settings) {
            const auto& before=impact.before.settings;
            const auto& after=*request.settings;
            text<<L"Applying these settings restarts the ROM.\r\n"
                <<L"Cartridge RAM: "<<before.ramBytes<<L" -> "<<after.ramBytes<<L" bytes.\r\n";
            if(before.mapper!=after.mapper)
                text<<L"Changing the mapper discards existing cartridge RAM and initializes the new RAM to zero.\r\n";
            else if(before.ramBytes>after.ramBytes)
                text<<L"The last "<<(before.ramBytes-after.ramBytes)<<L" bytes will be discarded.\r\n";
            else if(before.ramBytes<after.ramBytes)
                text<<L"Additional RAM is initialized to zero.\r\n";
        } else text<<L"Only the display name changes. The ROM keeps running.\r\n";
        constexpr const wchar_t* labels[]{L"Keep",L"Convert",L"Delete"};
        for(auto disposition:{StateDisposition::Keep,StateDisposition::Convert,StateDisposition::Delete}) {
            text<<L"\r\n"<<labels[static_cast<size_t>(disposition)]<<L" ("
                <<std::ranges::count(impact.states,disposition,&StateImpact::disposition)<<L")\r\n";
            for(const auto& entry:impact.states) if(entry.disposition==disposition)
                text<<L"#"<<entry.state.id<<L"  "<<(entry.state.name.empty()?L"(unnamed)":entry.state.name)<<L"\r\n";
        }
        text_=text.str();
    }
    BEGIN_MSG_MAP(RomImpactDialog)
        MESSAGE_HANDLER(WM_INITDIALOG,initialize)
        COMMAND_ID_HANDLER(IDOK,finish)
        COMMAND_ID_HANDLER(IDCANCEL,finish)
    END_MSG_MAP()
private:
    LRESULT initialize(UINT,WPARAM,LPARAM,BOOL&) {
        SetDlgItemText(IDC_ROM_IMPACT,text_.c_str());
        return TRUE;
    }
    LRESULT finish(WORD,WORD id,HWND,BOOL&) { EndDialog(id); return 0; }
    std::wstring text_;
};
class StateManagerDialog final : public ATL::CDialogImpl<StateManagerDialog> {
public:
    enum { IDD=IDD_MANAGE_STATES };
    StateManagerDialog(const std::vector<StateSummary>& states,bool canSave) : states_(states),canSave_(canSave) {}
    std::optional<int64_t> stateId;
    std::wstring name;

    BEGIN_MSG_MAP(StateManagerDialog)
        MESSAGE_HANDLER(WM_INITDIALOG,initialize)
        COMMAND_HANDLER(IDC_STATE_LIST,LBN_SELCHANGE,selectionChanged)

        COMMAND_ID_HANDLER(IDC_STATE_OVERWRITE,act)
        COMMAND_ID_HANDLER(IDC_STATE_RESTORE,act)
        COMMAND_ID_HANDLER(IDC_STATE_DELETE,act)
        COMMAND_ID_HANDLER(IDCANCEL,act)
    END_MSG_MAP()
private:
    static std::wstring label(const StateSummary& state) {
        return std::to_wstring(state.slot)+L"  "+(state.id?timestamp(state.updatedAt)+(state.name.empty()?L"":L" "+state.name):L"(Empty)");
    }
    static std::wstring timestamp(int64_t seconds) {
        std::tm local{};
        wchar_t text[32]{};
        if(_localtime64_s(&local,&seconds)!=0 || !std::wcsftime(text,std::size(text),L"%Y-%m-%d %H:%M:%S",&local))
            return L"Unknown";
        return text;
    }
    const StateSummary* selected() {
        const auto row=SendDlgItemMessage(IDC_STATE_LIST,LB_GETCURSEL,0,0);
        return row==LB_ERR || !states_[static_cast<size_t>(row)].id?nullptr:&states_[static_cast<size_t>(row)];
    }
    void showSelection() {
        const auto* state=selected();
        ::EnableWindow(GetDlgItem(IDC_STATE_OVERWRITE),state && canSave_); SetDlgItemText(IDC_STATE_NAME,state?state->name.c_str():L"");
        ::EnableWindow(GetDlgItem(IDC_STATE_RESTORE),state!=nullptr);
        ::EnableWindow(GetDlgItem(IDC_STATE_DELETE),state!=nullptr);
        const auto details=state?L"Created: "+timestamp(state->createdAt)+L"    Updated: "+timestamp(state->updatedAt)+L" (local)":
            states_.empty()?L"No saved states.":L"Select a slot to edit its title, restore, or delete.";
        SetDlgItemText(IDC_STATE_DETAILS,details.c_str());
    }
    LRESULT initialize(UINT,WPARAM,LPARAM,BOOL&) {
        for(size_t i=0;i<states_.size();++i) {
            const auto row=label(states_[i]);
            if(SendDlgItemMessage(IDC_STATE_LIST,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.c_str()))!=static_cast<LRESULT>(i)) {
                EndDialog(-1);
                return TRUE;
            }
        }
        ::EnableWindow(GetDlgItem(IDC_STATE_NAME),canSave_);

        showSelection();
        return TRUE;
    }
    LRESULT selectionChanged(WORD,WORD,HWND,BOOL&) { showSelection(); return 0; }
    LRESULT act(WORD,WORD id,HWND,BOOL&) {
        if(id==IDCANCEL) {EndDialog(IDCANCEL);return 0;}
        const auto* state=selected();
        if(!state || (id==IDC_STATE_OVERWRITE && !canSave_)) return 0;
        if(id==IDC_STATE_DELETE) {
            const auto message=L"Delete state "+label(*state)+L"?";
            if(::MessageBoxW(m_hWnd,message.c_str(),L"Delete state",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES) return 0;
        }
        stateId=state->id;
        if(id==IDC_STATE_OVERWRITE) {
            const auto input=GetDlgItem(IDC_STATE_NAME);
            name.resize(::GetWindowTextLengthW(input)+1);
            name.resize(::GetWindowTextW(input,name.data(),static_cast<int>(name.size())));
        }
        EndDialog(id);
        return 0;
    }
    const std::vector<StateSummary>& states_;
    bool canSave_;
};
}
class XInputShortcutsDialog final : public ATL::CDialogImpl<XInputShortcutsDialog> {
public:
    enum { IDD=IDD_XINPUT_SHORTCUTS };
    XInputShortcutsDialog(const XInputController& gamepad,unsigned slot,ShortcutBindings& shortcuts)
        : gamepad_(gamepad),slot_(slot),shortcuts_(shortcuts) {}

    BEGIN_MSG_MAP(XInputShortcutsDialog)
        MESSAGE_HANDLER(WM_INITDIALOG,initialize)
        MESSAGE_HANDLER(WM_TIMER,poll)
        COMMAND_ID_HANDLER(IDOK,accept)
        COMMAND_ID_HANDLER(IDCANCEL,cancel)
    END_MSG_MAP()
private:
    LRESULT initialize(UINT,WPARAM,LPARAM,BOOL&) {
        for(size_t i=0;i<shortcuts_.size();++i) {
            const auto id=IDC_SHORTCUT_BASE+static_cast<int>(i);
            for(const auto& button:shortcutButtons)
                SendDlgItemMessage(id,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(button.label));
            const auto choice=std::ranges::find(shortcutButtons,shortcuts_[i],&XInputButtonChoice::mask);
            SendDlgItemMessage(id,CB_SETCURSEL,choice-shortcutButtons.begin(),0);
        }
        if(!::SetTimer(m_hWnd,1,16,nullptr)) EndDialog(-1);
        return TRUE;
    }
    LRESULT poll(UINT,WPARAM,LPARAM,BOOL&) {
        const auto state=gamepad_.poll(slot_);
        const auto previous=std::exchange(previousButtons_,state.buttons);
        if(!state.connected || ::GetActiveWindow()!=m_hWnd) {
            armed_=false;
            return 0;
        }
        if(!state.buttons) armed_=true;
        if(!armed_ || !std::has_single_bit(state.buttons) || (previous&state.buttons)) return 0;
        const auto chosen=std::ranges::find(shortcutButtons,state.buttons,&XInputButtonChoice::mask);
        if(chosen==shortcutButtons.end()) return 0;
        for(size_t i=0;i<shortcuts_.size();++i) {
            const auto id=IDC_SHORTCUT_BASE+static_cast<int>(i);
            if(::GetFocus()!=GetDlgItem(id) || SendDlgItemMessage(id,CB_GETDROPPEDSTATE)!=0) continue;
            SendDlgItemMessage(id,CB_SETCURSEL,chosen-shortcutButtons.begin(),0);
            ::SetFocus(GetDlgItem(i+1<shortcuts_.size()?id+1:IDOK));
            break;
        }
        return 0;
    }
    LRESULT accept(WORD,WORD,HWND,BOOL&) {
        auto bindings=shortcuts_;
        for(size_t i=0;i<bindings.size();++i) {
            const auto choice=comboChoice(m_hWnd,IDC_SHORTCUT_BASE+static_cast<int>(i),shortcutButtons.size());
            if(!choice) return 0;
            bindings[i]=shortcutButtons[*choice].mask;
        }
        shortcuts_=bindings;
        EndDialog(IDOK);
        return 0;
    }
    LRESULT cancel(WORD,WORD,HWND,BOOL&) {EndDialog(IDCANCEL);return 0;}
    const XInputController& gamepad_;
    unsigned slot_;
    ShortcutBindings& shortcuts_;
    unsigned previousButtons_{};
    bool armed_{};
};

class MainWindow::SettingsDialog final : public ATL::CDialogImpl<SettingsDialog> {
public:
    enum { IDD=IDD_SETTINGS };
    explicit SettingsDialog(MainWindow& owner)
        : owner_(owner),next_(owner.romDefaults_),controllers_(owner.input_.configuration()),
          shortcuts_(owner.input_.shortcuts()),backends_{std::filesystem::path{},bundledFmPlugin} {
        backends_.insert(backends_.end(),owner.fmPlugins_.begin(),owner.fmPlugins_.end());
    }
    const std::exception_ptr& initializationFailure() const { return initializationFailure_; }

    BEGIN_MSG_MAP(SettingsDialog)
        MESSAGE_HANDLER(WM_INITDIALOG,initialize)
        MESSAGE_HANDLER(WM_TIMER,pollAssignment)
        NOTIFY_HANDLER(IDC_SETTINGS_TABS,TCN_SELCHANGE,tabChanged)
        COMMAND_HANDLER(IDC_ROM_MACHINE,CBN_SELCHANGE,profileChanged)
        COMMAND_HANDLER(IDC_ROM_VIDEO,CBN_SELCHANGE,profileChanged)
        COMMAND_HANDLER(IDC_ROM_MAPPER,CBN_SELCHANGE,profileChanged)
        COMMAND_HANDLER(IDC_ROM_RAM,CBN_SELCHANGE,profileChanged)
        COMMAND_HANDLER(IDC_ROM_FM,BN_CLICKED,profileChanged)
        COMMAND_HANDLER(IDC_PAD_PLAYER,CBN_SELCHANGE,padChanged)
        COMMAND_HANDLER(IDC_PAD_SOURCE,CBN_SELCHANGE,padChanged)
        COMMAND_HANDLER(IDC_PAD_SLOT,CBN_SELCHANGE,padChanged)
        COMMAND_RANGE_HANDLER(IDC_PAD_BUTTON_BASE,IDC_PAD_BUTTON_BASE+3,buttonChanged)
        COMMAND_RANGE_HANDLER(IDC_PAD_CLEAR_RAPID_BASE,IDC_PAD_CLEAR_RAPID_BASE+1,clearRapidKey)
        COMMAND_ID_HANDLER(IDC_PAD_SHORTCUTS,editShortcuts)
        COMMAND_ID_HANDLER(IDOK,accept)
        COMMAND_ID_HANDLER(IDCANCEL,cancel)
    END_MSG_MAP()
private:
    void add(int id,const wchar_t* label) {
        SendDlgItemMessage(id,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    }
    void select(int id,size_t value) {SendDlgItemMessage(id,CB_SETCURSEL,value,0);}

    LRESULT initialize(UINT,WPARAM,LPARAM,BOOL&) try {
        const auto tabs=GetDlgItem(IDC_SETTINGS_TABS);
        wchar_t defaults[]=L"New ROM Defaults", pad[]=L"Controller Configuration";
        TCITEMW item{.mask=TCIF_TEXT,.pszText=defaults};
        TabCtrl_InsertItem(tabs,0,&item);
        item.pszText=pad;
        TabCtrl_InsertItem(tabs,1,&item);
        for(const auto label:{L"SMS",L"Game Gear",L"SG-1000",L"Auto"}) add(IDC_ROM_MACHINE,label);
        for(const auto label:{L"NTSC",L"PAL"}) add(IDC_ROM_VIDEO,label);
        for(const auto label:{L"ROM Only",L"Sega",L"Codemasters",L"Auto"}) add(IDC_ROM_MAPPER,label);
        for(const auto label:{L"None",L"Strip 512 bytes"}) add(IDC_ROM_COPIER_HEADER,label);
        add(IDC_ROM_BACKEND,L"Built-in emu2413");
        add(IDC_ROM_BACKEND,L"ymfm OPLL (bundled DLL)");
        for(const auto& plugin:owner_.fmPlugins_) add(IDC_ROM_BACKEND,plugin.c_str());
        for(const auto method:{FmInterpolation::Sinc,FmInterpolation::ZohLowPass,FmInterpolation::Linear})
            add(IDC_ROM_INTERPOLATION,interpolationName(method));
        select(IDC_ROM_MACHINE,next_.machine?static_cast<size_t>(*next_.machine):autoMachineIndex);
        select(IDC_ROM_COPIER_HEADER,static_cast<size_t>(next_.options.copierHeader));
        const auto backend = std::ranges::find(backends_, next_.fmPlugin);
        if (backend != backends_.end() && fmBackendAvailable(*backend))
            select(IDC_ROM_BACKEND, backend - backends_.begin());
        else SetDlgItemText(IDC_ROM_ERROR, missingFmBackend);
        select(IDC_ROM_INTERPOLATION,static_cast<size_t>(next_.fmMethod));
        CheckDlgButton(IDC_ROM_FM,next_.fmEnabled?BST_CHECKED:BST_UNCHECKED);
        updateProfile();
        add(IDC_PAD_PLAYER,L"Player 1");add(IDC_PAD_PLAYER,L"Player 2");
        select(IDC_PAD_PLAYER,0);
        for(const auto label:{L"1",L"2",L"3",L"4"}) add(IDC_PAD_SLOT,label);
        for(size_t i=0;i<controllers_[0].keys.size();++i)
            requireWin32(::SetWindowSubclass(GetDlgItem(IDC_PAD_KEY_BASE+i),assignmentKey,1,reinterpret_cast<DWORD_PTR>(this)),"Subclass key field");
        for(size_t i=0;i<controllers_[0].buttons.size();++i) {
            const auto id=IDC_PAD_BUTTON_BASE+static_cast<int>(i);
            for(const auto& choice:xinputButtons) add(id,choice.label);
            if(i>=2) add(id,L"(Unassigned)");
            requireWin32(::SetWindowSubclass(GetDlgItem(id),assignmentKey,1,reinterpret_cast<DWORD_PTR>(this)),"Subclass button field");
        }
        showTab();
        requireWin32(::SetTimer(m_hWnd,1,16,nullptr)!=0,"Set controller assignment timer");
        return TRUE;
    } catch (const std::exception&) {
        initializationFailure_ = std::current_exception();
        EndDialog(-1);
        return FALSE;
    }

    void showControl(int id,bool show) { ::ShowWindow(GetDlgItem(id),show?SW_SHOWNA:SW_HIDE); }

    void showTab() {
        controllerTab_=TabCtrl_GetCurSel(GetDlgItem(IDC_SETTINGS_TABS))==1;
        for(const auto id:{IDC_ROM_MACHINE,IDC_ROM_VIDEO,IDC_ROM_MAPPER,IDC_ROM_RAM,IDC_ROM_COPIER_HEADER,
                IDC_ROM_FM,IDC_ROM_BACKEND,IDC_ROM_INTERPOLATION}) showControl(id,!controllerTab_);
        for(int i=0;i<7;++i) showControl(IDC_ROM_LABEL_BASE+i,!controllerTab_);
        showControl(IDC_ROM_ERROR,!controllerTab_);
        showControl(IDC_PAD_ERROR,controllerTab_);
        for(const auto id:{IDC_PAD_LABEL_PLAYER,IDC_PAD_PLAYER,IDC_PAD_LABEL_SOURCE,IDC_PAD_SOURCE})
            showControl(id,controllerTab_);
        refreshPad();
    }

    LRESULT tabChanged(int,LPNMHDR,BOOL&) { showTab();return 0; }

    void refreshPad() {
        const auto& pad=controllers_[player_];
        const bool keyboard=controllerTab_ && pad.source==PadSource::Keyboard;
        const bool xinput=controllerTab_ && pad.source==PadSource::XInput;
        showControl(IDC_PAD_SHORTCUTS,xinput && player_==0);
        SendDlgItemMessage(IDC_PAD_SOURCE,CB_RESETCONTENT,0,0);
        if(player_) add(IDC_PAD_SOURCE,L"Disabled");
        add(IDC_PAD_SOURCE,L"Keyboard");add(IDC_PAD_SOURCE,L"XInput");
        select(IDC_PAD_SOURCE,static_cast<unsigned>(pad.source)-(player_==0));
        select(IDC_PAD_SLOT,pad.slot);
        for(const auto id:{IDC_PAD_LABEL_SLOT,IDC_PAD_SLOT,IDC_PAD_STATUS}) showControl(id,xinput);
        for(size_t i=0;i<pad.keys.size();++i) {
            showControl(IDC_PAD_LABEL_KEY_BASE+i,keyboard || (xinput && i>=4));
            showControl(IDC_PAD_KEY_BASE+i,keyboard);
            wchar_t name[64]{};
            const auto key=pad.keys[i];
            if(key) {
                const auto sc=::MapVirtualKeyW(key,MAPVK_VK_TO_VSC_EX);
                const bool extended=(sc&0xff00) || (key>=VK_PRIOR && key<=VK_DOWN) || key==VK_INSERT || key==VK_DELETE;
                ::GetKeyNameTextW(((sc&0xff)<<16) | (extended?0x1000000:0),name,_countof(name));
            }
            const auto text=name[0]?std::wstring(name):key?L"Key "+std::to_wstring(key):L"(Unassigned)";
            SetDlgItemText(IDC_PAD_KEY_BASE+i,text.c_str());
        }
        for(size_t i=0;i<pad.buttons.size();++i) {
            const auto id=IDC_PAD_BUTTON_BASE+static_cast<int>(i);
            showControl(id,xinput);
            select(id,std::ranges::find(xinputButtons,pad.buttons[i],&XInputButtonChoice::mask)-xinputButtons.begin());
        }
        for(int i=0;i<2;++i) showControl(IDC_PAD_CLEAR_RAPID_BASE+i,keyboard);
        const auto state=xinput?owner_.gamepad_.poll(pad.slot):GamepadState{};
        previousButtons_=state.buttons&~(leftTriggerButton|rightTriggerButton);
        connected_=state.connected;
        if(xinput) updateConnectionLabel();
    }

    void updateConnectionLabel() {
        const auto text=L"Slot "+std::to_wstring(controllers_[player_].slot+1)
            +(connected_?L" connected":L" disconnected");
        SetDlgItemText(IDC_PAD_STATUS,text.c_str());
    }

    LRESULT padChanged(WORD,WORD id,HWND,BOOL&) {
        const auto index = comboChoice(m_hWnd, id, id == IDC_PAD_PLAYER ? controllers_.size() :
            id == IDC_PAD_SOURCE ? (player_ == 0 ? 2u : 3u) : 4u);
        if (!index) return 0;
        if(id==IDC_PAD_PLAYER) player_=*index;
        if(id==IDC_PAD_SOURCE) controllers_[player_].source=static_cast<PadSource>(*index+(player_==0));
        if(id==IDC_PAD_SLOT) controllers_[player_].slot=static_cast<unsigned>(*index);
        SetDlgItemText(IDC_PAD_ERROR,L"");
        refreshPad();
        return 0;
    }

    LRESULT buttonChanged(WORD code,WORD id,HWND,BOOL&) {
        if(code==CBN_SELCHANGE) {
            const auto button = id - IDC_PAD_BUTTON_BASE;
            const auto index = comboChoice(m_hWnd, id, xinputButtons.size() + (button >= 2));
            if (!index) return 0;
            controllers_[player_].buttons[button]=*index<xinputButtons.size()?xinputButtons[*index].mask:0;
            SetDlgItemText(IDC_PAD_ERROR,L"");
        }
        return 0;
    }

    LRESULT clearRapidKey(WORD,WORD id,HWND,BOOL&) {
        controllers_[player_].keys[6+id-IDC_PAD_CLEAR_RAPID_BASE]=0;
        SetDlgItemText(IDC_PAD_ERROR,L"");
        refreshPad();
        return 0;
    }

    LRESULT editShortcuts(WORD,WORD,HWND,BOOL&) {
        XInputShortcutsDialog dialog(owner_.gamepad_,controllers_[0].slot,shortcuts_);
        if(dialog.DoModal(m_hWnd)==-1) SetDlgItemText(IDC_PAD_ERROR,L"Could not open XInput shortcuts.");
        else SetDlgItemText(IDC_PAD_ERROR,L"");
        refreshPad();
        return 0;
    }

    LRESULT pollAssignment(UINT,WPARAM,LPARAM,BOOL&) {
        auto& pad=controllers_[player_];
        if(!controllerTab_ || pad.source!=PadSource::XInput) return 0;
        const auto state=owner_.gamepad_.poll(pad.slot);
        const auto buttons=state.buttons&~(leftTriggerButton|rightTriggerButton);
        const auto previous=std::exchange(previousButtons_,buttons);
        const bool wasConnected=std::exchange(connected_,state.connected);
        if(connected_!=wasConnected) updateConnectionLabel();
        if(!connected_ || !wasConnected || !std::has_single_bit(buttons) || (previous&buttons)) return 0;
        const auto button=std::ranges::find(xinputButtons,buttons,&XInputButtonChoice::mask);
        if(button==xinputButtons.end()) return 0;
        for(size_t i=0;i<pad.buttons.size();++i) {
            const auto id=IDC_PAD_BUTTON_BASE+static_cast<int>(i);
            if(::GetFocus()!=GetDlgItem(id)) continue;
            pad.buttons[i]=button->mask;
            SetDlgItemText(IDC_PAD_ERROR,L"");
            select(id,button-xinputButtons.begin());
            ::SetFocus(GetDlgItem(i+1<pad.buttons.size()?id+1:IDOK));
            break;
        }
        return 0;
    }

    static LRESULT CALLBACK assignmentKey(HWND field,UINT message,WPARAM key,LPARAM detail,UINT_PTR id,DWORD_PTR owner) {
        if (message==WM_NCDESTROY) {
            ::RemoveWindowSubclass(field,assignmentKey,id);
            return ::DefSubclassProc(field,message,key,detail);
        }
        auto& dialog=*reinterpret_cast<SettingsDialog*>(owner);
        const auto index=::GetDlgCtrlID(field)-IDC_PAD_KEY_BASE;
        auto& keys=dialog.controllers_[dialog.player_].keys;
        const bool keyboard=index>=0 && static_cast<size_t>(index)<keys.size();
        if(message==WM_GETDLGCODE && detail) {
            const auto* next=reinterpret_cast<const MSG*>(detail);
            if(next->wParam==VK_ESCAPE || (keyboard && next->wParam!=VK_TAB && next->wParam!=VK_RETURN))
                return ::DefSubclassProc(field,message,key,detail)|DLGC_WANTMESSAGE;
        }
        if(message==WM_KEYDOWN && key==VK_ESCAPE && !::SendMessageW(field,CB_GETDROPPEDSTATE,0,0)) {
            ::SetFocus(dialog.GetDlgItem(IDC_PAD_SOURCE));
            return 0;
        }
        if(message==WM_KEYDOWN && keyboard && key!=VK_TAB && key!=VK_RETURN) {
            const auto candidate=static_cast<unsigned>(key);
            if((detail&(LPARAM{1}<<30)) || !assignablePadKey(candidate)) return 0;
            BYTE held[256]{};
            ::GetKeyboardState(held);
            // The IME can report Kanji mode as held independently of the keypress.
            held[VK_KANJI]=0;
            for(unsigned other=0;other<std::size(held);++other) {
                if(other!=candidate && (held[other]&0x80) && (assignablePadKey(other) ||
                    other==VK_CONTROL || other==VK_SHIFT || other==VK_MENU)) return 0;
            }
            keys[index]=candidate;
            dialog.SetDlgItemText(IDC_PAD_ERROR,L"");
            dialog.refreshPad();
            ::SetFocus(dialog.GetDlgItem(static_cast<size_t>(index+1)<keys.size()?IDC_PAD_KEY_BASE+index+1:IDOK));
            return 0;
        }
        return ::DefSubclassProc(field,message,key,detail);
    }

    void updateProfile() {
        const auto options=next_.effectiveOptions();
        select(IDC_ROM_VIDEO,static_cast<size_t>(next_.effectiveVideo()));
        select(IDC_ROM_MAPPER,static_cast<size_t>(options.mapper));
        SendDlgItemMessage(IDC_ROM_RAM,CB_RESETCONTENT,0,0);
        constexpr const wchar_t* labels[]{L"None",L"8 KiB",L"16 KiB",L"32 KiB"};
        for(size_t i=0;i<std::size(ramSizes);++i)
            if(compatibleRamCapacity(options.mapper,ramSizes[i])==ramSizes[i]) add(IDC_ROM_RAM,labels[i]);
        select(IDC_ROM_RAM,std::ranges::find(ramSizes,options.sramBytes)-std::begin(ramSizes));

        const bool sg=next_.machine==Machine::Sg1000;
        ::EnableWindow(GetDlgItem(IDC_ROM_VIDEO),!sg && next_.machine!=Machine::GameGear);
        ::EnableWindow(GetDlgItem(IDC_ROM_MAPPER),!sg);
        ::EnableWindow(GetDlgItem(IDC_ROM_RAM),!sg && options.mapper!=MapperKind::RomOnly);
        const bool fm=canUseFm(next_.machine,next_.standard);
        ::EnableWindow(GetDlgItem(IDC_ROM_FM),fm);
        for(const auto id:{IDC_ROM_BACKEND,IDC_ROM_INTERPOLATION})
            ::EnableWindow(GetDlgItem(id),fm && next_.fmEnabled);
    }

    LRESULT profileChanged(WORD,WORD id,HWND,BOOL&) {
        if (id == IDC_ROM_FM) {
            next_.fmEnabled = IsDlgButtonChecked(id) == BST_CHECKED;
        } else {
            const auto count = id == IDC_ROM_MACHINE ? autoMachineIndex + 1 : id == IDC_ROM_VIDEO ? 2u :
                id == IDC_ROM_MAPPER ? static_cast<unsigned>(MapperKind::Auto) + 1 : std::size(ramSizes);
            const auto choice = comboChoice(m_hWnd, id, count);
            if (!choice) return 0;
            switch (id) {
            case IDC_ROM_MACHINE:
                if (*choice == autoMachineIndex) next_.machine.reset();
                else next_.machine = static_cast<Machine>(*choice);
                break;
            case IDC_ROM_VIDEO: next_.standard = static_cast<VideoStandard>(*choice); break;
            case IDC_ROM_MAPPER: next_.options.mapper = static_cast<MapperKind>(*choice); break;
            case IDC_ROM_RAM: next_.options.sramBytes = ramSizes[*choice]; return 0;
            }
        }
        next_.normalize();
        updateProfile();
        return 0;
    }

    LRESULT accept(WORD,WORD,HWND,BOOL&) {
        if(const auto error=validateControllerConfiguration(controllers_)) {
            TabCtrl_SetCurSel(GetDlgItem(IDC_SETTINGS_TABS),1);
            showTab();
            SetDlgItemText(IDC_PAD_ERROR,error);
            ::SetFocus(GetDlgItem(IDC_PAD_PLAYER));
            return 0;
        }
        if(const auto* operation=validateShortcuts(shortcuts_,controllers_[0])) {
            player_=0;
            select(IDC_PAD_PLAYER,0);
            TabCtrl_SetCurSel(GetDlgItem(IDC_SETTINGS_TABS),1);
            showTab();
            const auto message=std::wstring(L"XInput shortcut conflict: ")+operation+L".";
            SetDlgItemText(IDC_PAD_ERROR,message.c_str());
            ::SetFocus(GetDlgItem(IDC_PAD_SHORTCUTS));
            return 0;
        }
        const auto header = comboChoice(m_hWnd, IDC_ROM_COPIER_HEADER, 2);
        const auto interpolation = comboChoice(m_hWnd, IDC_ROM_INTERPOLATION, 3);
        const auto backend = comboChoice(m_hWnd, IDC_ROM_BACKEND, backends_.size());
        const bool available = backend && fmBackendAvailable(backends_[*backend]);
        if (!header || !interpolation || (!available && next_.fmEnabled && canUseFm(next_.machine, next_.standard))) {
            TabCtrl_SetCurSel(GetDlgItem(IDC_SETTINGS_TABS),0);
            showTab();
            SetDlgItemText(IDC_ROM_ERROR, !header || !interpolation ?
                L"Select a copier header and FM interpolation." : missingFmBackend);
            return 0;
        }
        next_.options.copierHeader=static_cast<CopierHeader>(*header);
        if (available) next_.fmPlugin=backends_[*backend];
        next_.fmMethod=static_cast<FmInterpolation>(*interpolation);
        if(owner_.saveSettings(next_,controllers_,shortcuts_,m_hWnd)) EndDialog(IDOK);
        return 0;
    }
    LRESULT cancel(WORD,WORD,HWND,BOOL&) {EndDialog(IDCANCEL);return 0;}

    MainWindow& owner_;
    RomDefaults next_;
    ControllerConfiguration controllers_;
    ShortcutBindings shortcuts_;
    std::exception_ptr initializationFailure_;
    size_t player_{};
    unsigned previousButtons_{};
    bool controllerTab_{},connected_{};
    std::vector<std::filesystem::path> backends_;
};

MainWindow::MainWindow(HINSTANCE instance, AudioStream& audio, std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath)), runner_(audio) {
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES};
        requireWin32(::InitCommonControlsEx(&controls),"Initialize Settings tabs");
        if(!databasePath_.empty()) romDatabase_=databasePath_;
        RECT bounds{0, 0, 768, 576};
        requireWin32(::AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, TRUE, 0, ::GetDpiForSystem()), "AdjustWindowRectExForDpi");
        RECT placement{CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT + bounds.right - bounds.left, CW_USEDEFAULT + bounds.bottom - bounds.top};
        WTL::CMenu menu(::LoadMenuW(instance, MAKEINTRESOURCEW(IDR_MAIN_MENU)));
        requireWin32(menu.m_hMenu != nullptr, "LoadMenuW");
        const auto directory=fmPluginDirectory();std::error_code error;
        const std::filesystem::directory_iterator files(directory,error);
        if(error && error!=std::errc::no_such_file_or_directory) throw std::filesystem::filesystem_error("List installed FM DLLs",directory,error);
        if(!error) for(const auto& file:files) {
            const auto name=file.path().filename();
            if(file.is_regular_file() && !_wcsicmp(name.extension().c_str(),L".dll") && _wcsicmp(name.c_str(),bundledFmPlugin)) fmPlugins_.push_back(name);
        }
        std::sort(fmPlugins_.begin(),fmPlugins_.end());
        const auto created = Create(nullptr, &placement, L"SMS/GG", WS_OVERLAPPEDWINDOW, 0, menu.m_hMenu);
        if (created) menu.Detach(); // The window owns its menu after successful creation.
        if (failure_) std::rethrow_exception(failure_);
        requireWin32(created != nullptr, "WTL frame Create");
        presenter_ = std::make_unique<D3D11Presenter>(m_hWnd);
        if (!databasePath_.empty()) readSettings();
        presenter_->uploadFrame(snapshot_.pixels);
        updateMenu();
        updateStateSlots();
        consumeSnapshot();
        requireWin32(::SetTimer(m_hWnd, inputTimerId, 16, nullptr) != 0, "SetTimer for XInput");
        ::DragAcceptFiles(m_hWnd, TRUE);
    } catch (const std::exception&) {
        // Destroy while every member is still alive; ATL then releases the HWND/thunk.
        runner_.stop();
        presenter_.reset();
        if (m_hWnd) DestroyWindow();
        throw;
    }
}

MainWindow::~MainWindow() {
    runner_.stop();
    presenter_.reset();
    if (m_hWnd) DestroyWindow();
}

LRESULT MainWindow::onDropFiles(UINT, WPARAM wParam, LPARAM, BOOL&) {
    const std::unique_ptr<std::remove_pointer_t<HDROP>, decltype(&::DragFinish)>
        drop(reinterpret_cast<HDROP>(wParam), ::DragFinish);
    if (::DragQueryFileW(drop.get(), 0xffffffff, nullptr, 0) != 1) return 0;
    const auto length = ::DragQueryFileW(drop.get(), 0, nullptr, 0);
    std::wstring name(length + 1, L'\0');
    const auto copied = ::DragQueryFileW(drop.get(), 0, name.data(), length + 1);
    if (!copied) return 0;
    name.resize(copied);
    const std::filesystem::path path(std::move(name));
    if (isRawRomPath(path) || isZipRomPath(path)) openRom(path);
    return 0;
}

void MainWindow::openRom(const std::filesystem::path& path) {
    if(!canOperate()) return;
    const auto& defaults = romDefaults_;
    LoadRequest request{path,defaults.effectiveOptions(),defaults.effectiveVideo(),defaults.machine,defaults.fmEnabled,defaults.fmMethod,defaults.fmPlugin};
    if(romDatabase_) {
        request.useDefaultDatabase=true;
        request.databasePath=*romDatabase_;
    } else request.fmEnabled = defaults.fmEnabled && canUseFm(request.machine,request.standard);
    submitRom(std::move(request));
}

bool MainWindow::canOperate() const {
    return !settingsOpen_ && !(cheatDialog_ && cheatDialog_->m_hWnd) && !snapshot_.busy &&
        !snapshot_.fatal && !snapshot_.closeReady && !closeRequested_;
}

void MainWindow::openCheatDialog(bool editor) {
    if(canOperate() && runner_.captureCheats()) {
        pendingCheatEditor_=editor;
        operationStarted(RunnerOperation::CaptureCheats);
    }
}

void MainWindow::operationStarted(RunnerOperation operation) {
    snapshot_.busy=true;snapshot_.operation=operation;operationText_.clear();updateMenu();
}

void MainWindow::submitRom(LoadRequest request) {
    if(!runner_.load(request)) return;
    pendingRom_=std::move(request);operationStarted(RunnerOperation::LoadRom);
}

void MainWindow::editRomSettings() {
    if(!canOperate() || !snapshot_.databaseRecord) return;
    const auto& record=*snapshot_.databaseRecord;
    const auto& saved=record.settings;
    const LoadRequest request{snapshot_.romPath,saved.loadOptions(),saved.video,saved.machine,
        saved.fmEnabled,saved.interpolation,saved.fmPlugin};
    const UnregisteredRom rom{record.identity,*snapshot_.romMetadata};
    RegisterRomDialog dialog(request,rom,fmPlugins_,&record);
    const auto result=dialog.DoModal(m_hWnd);
    if(result==-1) throw std::runtime_error("Cannot open ROM settings dialog");
    if(!canOperate()) return;
    if(result==IDC_ROM_CLEAR_SRAM) {
        if(runner_.clearDatabaseSram()) operationStarted(RunnerOperation::ClearSram);
        return;
    }
    if(result!=IDOK) return;

    RomUpdateRequest edit;
    if(saved!=dialog.registration.settings) edit.settings=std::move(dialog.registration.settings);
    edit.name=std::move(dialog.registration.name);
    if(!edit.settings && edit.name==record.name) return;
    edit.lastPath=record.lastPath;
    edit.preview=true;
    if(runner_.updateRom(edit)) {
        pendingEdit_=std::move(edit);
        operationStarted(RunnerOperation::PreviewRomUpdate);
    }
}

void MainWindow::requestState(const std::filesystem::path& path,bool save) {
    if(!canOperate() || !snapshot_.romMetadata || snapshot_.databaseRecord || (save && snapshot_.state==RunnerState::Faulted)) return;
    if(save?runner_.saveState(path):runner_.loadState(path)) operationStarted(save?RunnerOperation::SaveState:RunnerOperation::LoadState);
}

std::wstring MainWindow::stateSlotDetails(unsigned slot) const {
    const auto it=std::find_if(stateSlots_.begin(),stateSlots_.end(),[&](const auto& s){return s.slot==slot;});
    if(it==stateSlots_.end()) return L"(Empty)";
    std::tm local{};
    wchar_t date[32]{};
    _localtime64_s(&local,&it->updatedAt);
    std::wcsftime(date,std::size(date),L"%Y-%m-%d %H:%M:%S",&local);
    return it->name.empty()?date:std::wstring(date)+L" "+it->name;
}

void MainWindow::selectStateSlot(unsigned slot) {
    if(activeStateSlot_==slot) return;
    activeStateSlot_=slot;
    updateStateSlots();
    if(!snapshot_.databaseRecord) return;
    presenter_->setSlotNotice(L"Slot "+std::to_wstring(slot)+L" \u2014 "+stateSlotDetails(slot));
    slotNoticeUntil_=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    render(true);
}

void MainWindow::requestStateSlot(bool save) {
    if(!canOperate() || !snapshot_.databaseRecord) return;
    if(save) {
        if(runner_.saveDatabaseSlot(activeStateSlot_)) operationStarted(RunnerOperation::SaveState);
        return;
    }
    const auto it=std::find_if(stateSlots_.begin(),stateSlots_.end(),[&](const auto& s){return s.slot==activeStateSlot_;});
    if(it!=stateSlots_.end() && runner_.loadDatabaseState(it->id)) operationStarted(RunnerOperation::LoadState);
}

void MainWindow::updateStateSlots() {
    const auto menu=fullscreenMenu_.m_hMenu?fullscreenMenu_.m_hMenu: ::GetMenu(m_hWnd);
    const auto file=GetSubMenu(menu,0);
    HMENU slots=nullptr;
    for(int i=0;i<GetMenuItemCount(file);++i) {
        const auto sub=GetSubMenu(file,i);
        if(sub && GetMenuItemID(sub,0)==IDM_STATE_SLOT_BASE) { slots=sub;break; }
    }
    if(!slots) return;
    while(GetMenuItemCount(slots)>0) DeleteMenu(slots,0,MF_BYPOSITION);
    for(unsigned slot=1;slot<=10;++slot) {
        std::wstring text=std::to_wstring(slot)+L"  "+stateSlotDetails(slot);
        for(size_t at=0;(at=text.find(L'&',at))!=std::wstring::npos;at+=2) text.insert(at,1,L'&');
        AppendMenuW(slots,MF_STRING|(activeStateSlot_==slot?MF_CHECKED:0),
            IDM_STATE_SLOT_BASE+slot-1,text.c_str());
    }
}
void MainWindow::showStateManager() {
    std::vector<StateSummary> rows;
    for(unsigned slot=1;slot<=10;++slot) {
        auto it=std::find_if(stateSlots_.begin(),stateSlots_.end(),[&](const auto& s){return s.slot==slot;});
        if(it!=stateSlots_.end()) rows.push_back(*it);
        else { StateSummary empty;empty.slot=slot;rows.push_back(empty); }
    }
    StateManagerDialog dialog(rows,snapshot_.state!=RunnerState::Faulted);
    const auto result=dialog.DoModal(m_hWnd);
    if(result==-1) throw std::runtime_error("Cannot open state manager");
    if(!canOperate()) return;
    switch(result) {
    case IDC_STATE_OVERWRITE:
        if(runner_.renameDatabaseState(*dialog.stateId,std::move(dialog.name))) operationStarted(RunnerOperation::SaveState);
        break;
    case IDC_STATE_RESTORE:
        if(runner_.loadDatabaseState(*dialog.stateId)) operationStarted(RunnerOperation::LoadState);
        break;
    case IDC_STATE_DELETE:
        if(runner_.deleteDatabaseState(*dialog.stateId)) operationStarted(RunnerOperation::DeleteState);
        break;
    }
}

void MainWindow::chooseState(bool save) {
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    if(save) {
        Microsoft::WRL::ComPtr<IFileSaveDialog> saveDialog;
        requireHr(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&saveDialog)),"Create state save dialog");
        requireHr(saveDialog.As(&dialog),"Get state dialog");
    } else {
        Microsoft::WRL::ComPtr<IFileOpenDialog> openDialog;
        requireHr(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&openDialog)),"Create state open dialog");
        requireHr(openDialog.As(&dialog),"Get state dialog");
    }
    constexpr COMDLG_FILTERSPEC filters[]{{L"acesms state",L"*.state"},{L"All files",L"*.*"}};
    requireHr(dialog->SetFileTypes(2,filters),"Set state file filters");
    requireHr(dialog->SetDefaultExtension(L"state"),"Set state extension");
    const auto name=snapshot_.romPath.stem().wstring()+L".state";
    requireHr(dialog->SetFileName(name.c_str()),"Set state filename");
    DWORD options{};requireHr(dialog->GetOptions(&options),"Get state dialog options");
    requireHr(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|(save?FOS_OVERWRITEPROMPT:FOS_FILEMUSTEXIST)),"Set state dialog options");
    const auto result=dialog->Show(m_hWnd);if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    requireHr(result,"Show state dialog");Microsoft::WRL::ComPtr<IShellItem> item;
    requireHr(dialog->GetResult(&item),"Get state file");PWSTR raw{};
    requireHr(item->GetDisplayName(SIGDN_FILESYSPATH,&raw),"Get state path");
    const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> path(raw,&CoTaskMemFree);requestState(std::filesystem::path(path.get()),save);
}

void MainWindow::chooseRom() {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    requireHr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)), "Create file dialog");
    constexpr COMDLG_FILTERSPEC filters[]{{L"SMS/GG/SG-1000 ROM or ZIP", L"*.sms;*.gg;*.sg;*.zip"}, {L"All files", L"*.*"}};
    requireHr(dialog->SetFileTypes(2, filters), "Set file filters");
    DWORD options{};
    requireHr(dialog->GetOptions(&options), "Get dialog options");
    requireHr(dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM), "Set dialog options");
    const auto result = dialog->Show(m_hWnd);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    requireHr(result, "Open file dialog");
    Microsoft::WRL::ComPtr<IShellItem> item;
    requireHr(dialog->GetResult(&item), "Get selected ROM");
    PWSTR rawPath{};
    requireHr(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath), "Get ROM path");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);
    openRom(std::filesystem::path(path.get()));
}

LoadOptions RomDefaults::effectiveOptions() const {
    return machine == Machine::Sg1000 ? LoadOptions{MapperKind::RomOnly,0,options.copierHeader,Machine::Sg1000} : options;
}

VideoStandard RomDefaults::effectiveVideo() const {
    return machine == Machine::Sg1000 ? VideoStandard::Ntsc : standard;
}

void RomDefaults::normalize() {
    if (machine == Machine::GameGear) standard = VideoStandard::Ntsc;
    options.sramBytes=compatibleRamCapacity(options.mapper,options.sramBytes);
}

void MainWindow::readSettings() {
    auto opened=RomDatabase::open(databasePath_);
    if(const auto* error=std::get_if<DatabaseError>(&opened)) {startupSettingsError_=error->message;return;}
    auto loaded=std::get<RomDatabase>(opened).readAppSettings();
    if(const auto* error=std::get_if<DatabaseError>(&loaded)) {startupSettingsError_=error->message;return;}
    auto settings=std::get<AppSettings>(std::move(loaded));
    settings.rom.normalize();
    romDefaults_=std::move(settings.rom);
    if(settings.fourThree) presenter_->setAspectRatio(AspectRatio::FourThree);
    auto controllers=settings.controllers.value_or(defaultControllerConfiguration());
    if(validateControllerConfiguration(controllers)) startupSettingsError_="Invalid controller settings";
    else input_.configure(controllers,settings.shortcuts);
}

bool MainWindow::saveSettings(const RomDefaults& next,const ControllerConfiguration& controllers,
    const ShortcutBindings& shortcuts,HWND owner) {
    if(!databasePath_.empty()) {
        auto opened=RomDatabase::open(databasePath_);
        if(const auto* error=std::get_if<DatabaseError>(&opened)) {showSettingsError(error->message,owner);return false;}
        if(auto error=std::get<RomDatabase>(opened).saveAppSettings(next,controllers,shortcuts)) {
            showSettingsError(error->message,owner);
            return false;
        }
    }
    romDefaults_=next;
    input_.configure(controllers,shortcuts);
    return true;
}

void MainWindow::showSettings() {
    SettingsDialog dialog(*this);
    settingsOpen_=true;
    input_.clear();
    runner_.clearInput();
    runner_.setPaused(true);
    const auto result=dialog.DoModal(m_hWnd);
    settingsOpen_=false;
    runner_.setPaused(paused_);
    if (const auto& failure = dialog.initializationFailure()) {
        try { std::rethrow_exception(failure); }
        catch (const std::exception& error) { showSettingsError(error.what(), m_hWnd); }
    } else if (result == -1) showSettingsError("Could not open Settings dialog", m_hWnd);
}

void MainWindow::showSettingsError(std::string_view reason,HWND owner) {
    const auto message=L"Application settings\n"+databasePath_.wstring()+L"\n"+wide(reason);
    ::MessageBoxW(owner,message.c_str(),L"acesms",MB_OK|MB_ICONERROR);
}

void MainWindow::updateMenu() {
    const auto menu = fullscreenMenu_.m_hMenu ? fullscreenMenu_.m_hMenu : ::GetMenu(m_hWnd);
    requireWin32(menu != nullptr, "GetMenu");
    const auto enable=[&](unsigned id,bool value) {
        if(EnableMenuItem(menu,id,MF_BYCOMMAND|(value?MF_ENABLED:MF_GRAYED))==UINT(-1)) throw std::runtime_error("Set operation availability failed");
    };
    const bool available=canOperate();
    enable(IDM_FILE_OPEN,available);
    enable(IDM_EDIT_ROM,available && snapshot_.databaseRecord.has_value());
    enable(IDM_MANAGE_STATES,available && snapshot_.databaseRecord.has_value());
    const bool canCheat=available && snapshot_.databaseRecord.has_value() &&
        (snapshot_.state==RunnerState::Running || snapshot_.state==RunnerState::Paused);
    enable(IDM_MEMORY_SEARCH,canCheat);
    enable(IDM_CHEAT_EDITOR,canCheat);
    enable(IDM_SETTINGS,available);
    enable(IDM_ASPECT_SQUARE,available);
    enable(IDM_ASPECT_4_3,available);
    if (CheckMenuItem(menu, IDM_HOST_PAUSE, MF_BYCOMMAND | (paused_ ? MF_CHECKED : MF_UNCHECKED)) == DWORD(-1))
        throw std::runtime_error("Set pause menu failed");
    if (CheckMenuItem(menu, IDM_MUTE, MF_BYCOMMAND | (muted_ ? MF_CHECKED : MF_UNCHECKED)) == DWORD(-1))
        throw std::runtime_error("Set mute menu failed");
    if (CheckMenuItem(menu, IDM_FAST_FORWARD, MF_BYCOMMAND | (snapshot_.fastForward ? MF_CHECKED : MF_UNCHECKED)) == DWORD(-1))
        throw std::runtime_error("Set fast forward menu failed");
    if (presenter_) {
        const auto aspect = presenter_->aspectRatio() == AspectRatio::FourThree ? IDM_ASPECT_4_3 : IDM_ASPECT_SQUARE;
        requireWin32(::CheckMenuRadioItem(menu, IDM_ASPECT_SQUARE, IDM_ASPECT_4_3, aspect, MF_BYCOMMAND),
            "Set aspect ratio menu");
    }
}

void MainWindow::onCommand(unsigned id) {
    if(id>=IDM_STATE_SLOT_BASE && id<IDM_STATE_SLOT_BASE+10) {
        selectStateSlot(id-IDM_STATE_SLOT_BASE+1);
        return;
    }
    switch (id) {
    case IDM_FILE_OPEN: if(canOperate()) chooseRom(); return;
    case IDM_SETTINGS: if(canOperate()) showSettings(); return;
    case IDM_MEMORY_SEARCH: openCheatDialog(false);return;
    case IDM_CHEAT_EDITOR: openCheatDialog(true);return;
    case IDM_EDIT_ROM: editRomSettings(); return;
    case IDM_MANAGE_STATES:
        if(canOperate() && snapshot_.databaseRecord && runner_.listDatabaseStates()) { showStateManagerPending_=true;operationStarted(RunnerOperation::ListStates); }
        return;
    case IDM_FILE_EXIT: close(); return;
    case IDM_SAVE_STATE: if(canOperate() && snapshot_.romMetadata && !snapshot_.databaseRecord && snapshot_.state!=RunnerState::Faulted) chooseState(true); return;
    case IDM_LOAD_STATE: if(canOperate() && snapshot_.romMetadata && !snapshot_.databaseRecord) chooseState(false); return;
    case IDM_HOST_PAUSE: paused_ = !paused_; runner_.setPaused(paused_); break;
    case IDM_MUTE: muted_ = !muted_; runner_.setMuted(muted_); break;
    case IDM_FAST_FORWARD: fastForwardRequested_ = !fastForwardRequested_; runner_.setFastForward(fastForwardRequested_); break;
    case IDM_FULLSCREEN: toggleFullscreen(); return;
    case IDM_ASPECT_SQUARE:
    case IDM_ASPECT_4_3: {
        if(!canOperate()) return;
        const auto ratio = id == IDM_ASPECT_4_3 ? AspectRatio::FourThree : AspectRatio::SquarePixels;
        if (presenter_->aspectRatio() == ratio) return;
        if(!databasePath_.empty()) {
            auto opened=RomDatabase::open(databasePath_);
            if(const auto* error=std::get_if<DatabaseError>(&opened)) {showSettingsError(error->message,m_hWnd);return;}
            if(auto error=std::get<RomDatabase>(opened).saveAspectRatio(ratio==AspectRatio::FourThree)) {
                showSettingsError(error->message,m_hWnd);
                return;
            }
        }
        presenter_->setAspectRatio(ratio);
        updateMenu();
        render(true);
        return;
    }
    case IDM_HELP_ROM_INFO: {
        if(!snapshot_.romMetadata) {::MessageBoxW(m_hWnd,L"No ROM loaded",L"ROM information",MB_OK);return;}
        const auto& metadata=*snapshot_.romMetadata;
        std::wostringstream info;
        const auto videoName=[](VideoStandard video){return video==VideoStandard::Pal?L"PAL":L"NTSC";};
        const auto hexBytes=[&](const auto& bytes) {
            info<<std::hex<<std::setfill(L'0');
            for(auto byte:bytes) info<<std::setw(2)<<unsigned(byte);
            info<<std::dec;
        };
        if(const auto& record=snapshot_.databaseRecord) {
            const auto& saved=record->settings;
            info<<L"Database: "<<snapshot_.databasePath.wstring()<<L"\nROM ID: "<<record->id<<L" | Name: "<<record->name
                <<L"\nLast registered path: "<<record->lastPath.wstring()<<L"\nSHA-256: ";
            hexBytes(record->identity.sha256);
            info<<L"\nSHA-1: ";
            hexBytes(record->identity.sha1);
            info<<L"\nSaved settings: "<<machineName(saved.machine)<<L" / "<<videoName(saved.video)<<L" / "
                <<mapperName(saved.mapper)<<L" / "<<saved.ramBytes<<L" bytes\nSaved FM: ";
            if(saved.fmEnabled) info<<(saved.fmPlugin.empty()?L"Built-in emu2413":saved.fmPlugin.wstring())
                <<L" / "<<interpolationName(saved.interpolation);
            else info<<L"disabled";
            info<<L"\n\n";
        }
        info<<L"Running profile: "<<machineName(snapshot_.machine)<<L" / "<<videoName(snapshot_.video)
            <<L" / "<<snapshot_.sampleRate<<L" Hz\n";
        info<<L"Mapper: "<<mapperName(snapshot_.mapper)<<L"\nSelection: ";
        switch(metadata.mapperSelection) {
        case MapperSelection::Explicit:info<<L"explicit";break;
        case MapperSelection::CodemastersHeader:info<<L"Codemasters header/checksum matched";break;
        case MapperSelection::Linear48K:info<<L"48 KiB linear geometry";break;
        case MapperSelection::DefaultSega:info<<L"default Sega assumption (not detected)";break;
        }
        info<<L"\nSize: "<<metadata.size<<L" bytes\nCRC32: "<<std::hex<<std::uppercase<<std::setw(8)<<std::setfill(L'0')<<metadata.crc32;
        if(metadata.header) {
            const auto model=metadata.header->model;
            info<<L"\nSega header model: "<<(model==RomModel::Sms?L"SMS":model==RomModel::GameGear?L"GG":L"unknown")
                <<L"\nSega checksum: not checked (stored "<<metadata.header->checksum<<L")";
        } else info<<L"\nSega header: absent";
        if(metadata.codemasters) info<<L"\nCodemasters checksum: "
            <<(metadata.codemasters->status==ChecksumStatus::Match?L"matched":L"mismatch")
            <<L" (stored "<<metadata.codemasters->checksum<<L", computed "<<metadata.codemasters->computedChecksum<<L")";
        else info<<L"\nCodemasters header candidate: absent";
        info<<std::dec<<L"\nCartridge RAM: "<<snapshot_.ramBytes<<L" bytes";
        if(snapshot_.sramEnabled) info<<L"\nSega SRAM: "<<(snapshot_.databaseRecord?L"ROM database":snapshot_.sramPath.wstring());
        else if(snapshot_.mapper==MapperKind::Codemasters && snapshot_.ramBytes) info<<L"\nCodemasters RAM is volatile (not saved)";
        else info<<L"\nNo persistent SRAM";
        if(snapshot_.fmProfile) {
            const auto& fm=*snapshot_.fmProfile;
            info<<L"\nFM: "<<wide(snapshot_.fmBackendName)<<L"\nBackend ID: ";hexBytes(fm.backendId);
            info<<L"\nState version: "<<fm.stateVersion<<L"\nState compatibility ID: ";hexBytes(fm.compatibilityId);
            info<<L"\nFM clock/divider: "<<fm.clock<<L'/'<<fm.divider<<L"\nInterpolation: "<<interpolationName(fm.method)<<L" (version "<<fm.resamplerVersion<<L')';
        } else info<<L"\nFM: disabled";
        ::MessageBoxW(m_hWnd,info.str().c_str(),L"ROM information",MB_OK);return;
    }
    case IDM_HELP_ABOUT:
        ::MessageBoxW(m_hWnd,
            L"acesms " ACESMS_VERSION_WIDE L"\n"
            L"ACoward's Emulator Sega Master System\n\n"
            L"Copyright (c) 2026 ACoward\n"
            L"MIT License\n\n"
            L"https://github.com/ACoward-Kakuyomu/acesms",
            L"About acesms", MB_OK);
        return;
    default: return;
    }
    updateMenu();
}

void MainWindow::consumeSnapshot() {
    const auto previousState = snapshot_.state;
    const auto previousSpeed = snapshot_.fastForward;
    snapshot_=runner_.takeSnapshot();
    const bool completed=!snapshot_.busy && snapshot_.operationSerial!=handledSerial_;
    if(completed) handledSerial_=snapshot_.operationSerial;
    const bool stateRestored = completed && snapshot_.operation==RunnerOperation::LoadState && snapshot_.operationSucceeded;
    if(snapshot_.romChanged || stateRestored) {
        memorySearch_=MemorySearch{};
        input_.clear();runner_.clearInput();
    }
    if(fpsVisible_) updateFps(snapshot_.romChanged || stateRestored ||
        previousState!=snapshot_.state || previousSpeed!=snapshot_.fastForward);
    if(snapshot_.frameChanged) presenter_->uploadFrame(snapshot_.pixels,snapshot_.displayArea);
    if(completed) {
        constexpr const wchar_t* results[]{L"",L"ROM loaded",L"State saved",L"State loaded",L"SRAM saved",L"Ready to close",
            L"ROM settings updated",L"States listed",L"State deleted",L"",L"SRAM cleared",L"",L"Cheats saved",L"Cheat applied"};
        operationText_=snapshot_.unregisteredRom?L"":snapshot_.operationSucceeded?results[static_cast<size_t>(snapshot_.operation)]:L"Operation failed";
    }
    constexpr const wchar_t* states[]{L"Idle",L"Loading",L"Running",L"Paused",L"Stopped on error",L"Stopped"};
    constexpr const wchar_t* actions[]{L"",L"Loading ROM",L"Saving state",L"Loading state",L"Saving SRAM",L"Saving before exit",
        L"Applying ROM settings",L"Listing states",L"Deleting state",L"Checking ROM changes",L"Clearing SRAM",
        L"Capturing work RAM",L"Saving cheats",L"Applying cheat"};
    const std::wstring result=snapshot_.busy?actions[static_cast<size_t>(snapshot_.operation)]:operationText_;
    const auto title=snapshot_.romPath.filename().wstring()
        +(snapshot_.archiveEntry.empty()?L"":L" / "+snapshot_.archiveEntry.wstring())
        +L" | "+machineName(snapshot_.machine)+L" | "
        +states[static_cast<size_t>(snapshot_.state)]
        +(snapshot_.fastForward?L" | Fast forward":L"")
        +(snapshot_.fmEnabled?L" | FM "+wide(snapshot_.fmBackendName)+L" / "+interpolationName(snapshot_.fmProfile->method):L"")
        +(snapshot_.muted?L" | Muted":L" | Audio")
        +(snapshot_.romMetadata?std::wstring(L" | ")+mapperName(snapshot_.mapper)
            +(snapshot_.romMetadata->mapperSelection==MapperSelection::Explicit?L" / explicit":L" / auto"):L"")
        +(snapshot_.sramEnabled?L" | SRAM autosave":snapshot_.ramBytes?L" | Volatile RAM":L"")
        +(result.empty()?L"":L" | "+result)+L" - acesms";
    if(title!=title_) {requireWin32(::SetWindowTextW(m_hWnd,title.c_str()),"SetWindowTextW");title_=title;}
    updateMenu();render(false);
    if(snapshot_.fatal) {
        runner_.stop();
        if(!snapshot_.error.empty()) ::MessageBoxW(m_hWnd,wide(snapshot_.error).c_str(),L"acesms",MB_OK|MB_ICONERROR);
        finishClose();return;
    }
    if(completed && pendingCheatEditor_ && snapshot_.operation==RunnerOperation::CaptureCheats) {
        const bool editor=*pendingCheatEditor_;
        pendingCheatEditor_.reset();
        if(snapshot_.cheatSnapshot && !closeRequested_) {
            cheatDialog_=std::make_unique<CheatDialog>(runner_,memorySearch_,snapshot_.machine,
                snapshot_.databaseRecord->identity,*snapshot_.cheatSnapshot,editor);
            if(!cheatDialog_->Create(m_hWnd)) {
                runner_.setPaused(snapshot_.cheatSnapshot->wasPaused);
                throw std::runtime_error("Cannot open cheat dialog");
            }
            input_.clear();runner_.clearInput();
            ::EnableWindow(m_hWnd,FALSE);
            cheatDialog_->ShowWindow(SW_SHOW);
            updateMenu();
            return;
        }
        if(snapshot_.cheatSnapshot) runner_.setPaused(snapshot_.cheatSnapshot->wasPaused);
    }
    if(completed && cheatDialog_ && cheatDialog_->m_hWnd &&
        (snapshot_.operation==RunnerOperation::SaveCheats || snapshot_.operation==RunnerOperation::ApplyCheat)) {
        cheatDialog_->onResult(snapshot_,wide(snapshot_.error));
        updateMenu();
        return;
    }
    if(completed && snapshot_.unregisteredRom && pendingRom_ && !closeRequested_) {
        auto request=std::move(*pendingRom_);
        pendingRom_.reset();
        RegisterRomDialog dialog(request,*snapshot_.unregisteredRom,fmPlugins_);
        const auto result=dialog.DoModal(m_hWnd);
        if(result==-1) throw std::runtime_error("Cannot open ROM settings dialog");
        if(result==IDOK && canOperate()) {
            request.options.copierHeader=snapshot_.unregisteredRom->copierHeader;
            request.registration=std::move(dialog.registration);
            submitRom(std::move(request));
        }
        return;
    }
    if(completed && snapshot_.operation==RunnerOperation::PreviewRomUpdate && pendingEdit_) {
        auto edit=std::move(*pendingEdit_);
        pendingEdit_.reset();
        if(snapshot_.romUpdateImpact && !closeRequested_) {
            RomImpactDialog dialog(*snapshot_.romUpdateImpact,edit);
            const auto result=dialog.DoModal(m_hWnd);
            if(result==-1) throw std::runtime_error("Cannot open ROM changes dialog");
            if(result==IDOK && canOperate()) {
                edit.preview=false;
                edit.confirmedImpact=std::move(snapshot_.romUpdateImpact);
                if(runner_.updateRom(std::move(edit))) operationStarted(RunnerOperation::UpdateRom);
            }
            return;
        }
    }
    if(completed && snapshot_.operation==RunnerOperation::LoadRom && snapshot_.operationSucceeded) {
        activeStateSlot_=1;stateSlots_.clear();updateStateSlots();
        slotNoticeUntil_.reset();
        presenter_->setSlotNotice(L"");
    }
    if(completed && snapshot_.databaseStates) {
        stateSlots_=*snapshot_.databaseStates;
        updateStateSlots();
        if(snapshot_.savedStateId) {
            const auto it=std::find_if(stateSlots_.begin(),stateSlots_.end(),
                [&](const auto& s){return s.id==*snapshot_.savedStateId;});
            if(it!=stateSlots_.end() && it->slot) operationText_=L"State saved | Slot "+std::to_wstring(it->slot);
        }
    }
    if(completed && snapshot_.operation==RunnerOperation::ListStates && showStateManagerPending_ && snapshot_.databaseStates && canOperate()) {
        showStateManagerPending_=false;
        showStateManager();
        return;
    }
    const bool romSaveFailure=completed && snapshot_.operation==RunnerOperation::LoadRom && snapshot_.sramSaveFailed;
    if(completed && snapshot_.sramSaveFailed && (snapshot_.operation==RunnerOperation::PrepareClose || (romSaveFailure && !closeRequested_))) {
        const bool closing=snapshot_.operation==RunnerOperation::PrepareClose;
        const auto message=wide(snapshot_.error)+L"\n\nTry Again: retry saving.\nContinue: discard unsaved SRAM and "
            +(closing?L"close.":L"load the selected ROM.")+L"\nCancel: keep the current ROM open.";
        const auto choice=::MessageBoxW(m_hWnd,message.c_str(),L"SRAM was not saved",MB_CANCELTRYCONTINUE|MB_ICONWARNING);
        if(choice==IDTRYAGAIN || choice==IDCONTINUE) {
            const bool discard=choice==IDCONTINUE;
            if(closing) {if(runner_.prepareClose(discard)) operationStarted(RunnerOperation::PrepareClose);}
            else if(pendingRom_) {auto request=*pendingRom_;request.discardSram=discard;submitRom(std::move(request));}
        } else {closeRequested_=false;pendingRom_.reset();updateMenu();}
        return;
    }
    if(completed && snapshot_.operation==RunnerOperation::LoadRom) pendingRom_.reset();
    if(!snapshot_.error.empty() && !(romSaveFailure && closeRequested_)) ::MessageBoxW(m_hWnd,wide(snapshot_.error).c_str(),L"acesms",MB_OK|MB_ICONERROR);
    if(closeRequested_) {
        if(snapshot_.closeReady) finishClose();
        else if(!snapshot_.busy && runner_.prepareClose()) operationStarted(RunnerOperation::PrepareClose);
    }
}

void MainWindow::updateFps(bool reset) {
    if (!fpsVisible_ || snapshot_.state != RunnerState::Running) {
        fpsSampleTime_.reset();
        presenter_->setFpsText(fpsVisible_ && snapshot_.state == RunnerState::Paused ? "FPS 0.0" : "");
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (reset || !fpsSampleTime_) {
        fpsSampleTime_ = now;
        fpsSampleFrames_ = snapshot_.frames;
        presenter_->setFpsText("");
        return;
    }
    const auto elapsed = now - *fpsSampleTime_;
    if (elapsed < std::chrono::seconds{1}) return;
    const auto fps = (snapshot_.frames - fpsSampleFrames_) / std::chrono::duration<double>(elapsed).count();
    presenter_->setFpsText(std::format("FPS {:.1f}", fps));
    fpsSampleTime_ = now;
    fpsSampleFrames_ = snapshot_.frames;
}

void MainWindow::render(bool retryOccluded) {
    if (presenter_ && !minimized_) presenter_->present(retryOccluded,
        snapshot_.fastForward && snapshot_.state != RunnerState::Paused);
}

void MainWindow::toggleFullscreen() {
    constexpr auto frameFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
    const auto style = ::GetWindowLongW(m_hWnd, GWL_STYLE);
    if (fullscreenMenu_.m_hMenu) {
        requireWin32(::SetWindowLongW(m_hWnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW) != 0,
            "Restore window style");
        requireWin32(::SetMenu(m_hWnd, fullscreenMenu_.m_hMenu), "Restore window menu");
        fullscreenMenu_.Detach();
        refreshClientCursor(); // The restored window may move away from a stationary pointer.
        requireWin32(::SetWindowPlacement(m_hWnd, &windowedPlacement_), "Restore window placement");
        requireWin32(::SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0, frameFlags | SWP_NOMOVE | SWP_NOSIZE),
            "Refresh window frame");
        return;
    }
    requireWin32(::GetWindowPlacement(m_hWnd, &windowedPlacement_), "Save window placement");
    MONITORINFO monitor{sizeof(MONITORINFO)};
    requireWin32(::GetMonitorInfoW(::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor),
        "Get fullscreen monitor");
    if (::IsZoomed(m_hWnd)) ::ShowWindow(m_hWnd, SW_RESTORE);
    const auto menu = ::GetMenu(m_hWnd);
    requireWin32(::SetMenu(m_hWnd, nullptr), "Detach window menu");
    fullscreenMenu_.Attach(menu);
    const auto restoredStyle = ::GetWindowLongW(m_hWnd, GWL_STYLE);
    requireWin32(::SetWindowLongW(m_hWnd, GWL_STYLE, (restoredStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP) != 0,
        "Set borderless window style");
    const auto& rect = monitor.rcMonitor;
    requireWin32(::SetWindowPos(m_hWnd, nullptr, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, frameFlags), "Size fullscreen window");
    refreshClientCursor();
}

void MainWindow::cycleWindowScale(const RECT& workArea) {
    RECT client{}, window{};
    requireWin32(::GetClientRect(m_hWnd, &client), "Get current client size");
    requireWin32(::GetWindowRect(m_hWnd, &window), "Get current window bounds");
    const auto style = ::GetWindowLongW(m_hWnd, GWL_STYLE) & ~WS_MAXIMIZE;
    const auto exStyle = ::GetWindowLongW(m_hWnd, GWL_EXSTYLE);
    const auto dpi = ::GetDpiForWindow(m_hWnd);
    const auto area = snapshot_.displayArea;
    const bool fourThree = presenter_->aspectRatio() == AspectRatio::FourThree;
    SIZE selected{};
    for (unsigned scale = 1; scale <= 5; ++scale) {
        const auto height = static_cast<LONG>(area.height * scale);
        const auto width = fourThree ? height * 4 / 3 : static_cast<LONG>(area.width * scale);
        RECT frame{0, 0, width, height};
        requireWin32(::AdjustWindowRectExForDpi(&frame, style, TRUE, exStyle, dpi), "Adjust scaled window frame");
        // Ask Win32 how much extra height this width needs when the menu wraps.
        RECT measured = frame;
        measured.bottom = 32767;
        ::SendMessageW(m_hWnd, WM_NCCALCSIZE, FALSE, reinterpret_cast<LPARAM>(&measured));
        const SIZE size{frame.right - frame.left, frame.bottom - frame.top + measured.top};
        if (size.cx > workArea.right - workArea.left || size.cy > workArea.bottom - workArea.top) continue;
        if (selected.cx == 0) selected = size;
        if (height > client.bottom) {
            selected = size;
            break;
        }
    }
    if (selected.cx == 0) return;
    if (::IsZoomed(m_hWnd)) ::ShowWindow(m_hWnd, SW_RESTORE);
    // Keep the requested client width even if it is below the default tracking minimum (GG x1).
    requireWin32(::SetWindowPos(m_hWnd, nullptr,
        std::clamp(window.left, workArea.left, workArea.right - selected.cx),
        std::clamp(window.top, workArea.top, workArea.bottom - selected.cy),
        selected.cx, selected.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING),
        "Set scaled window bounds");
}

void MainWindow::close() {
    if(snapshot_.fatal) {finishClose();return;}
    if(cheatDialog_ && cheatDialog_->m_hWnd) {cheatDialog_->requestClose();updateMenu();return;}
    if(closeRequested_) return;
    closeRequested_=true;updateMenu();
    if(!snapshot_.busy && runner_.prepareClose()) operationStarted(RunnerOperation::PrepareClose);
}

void MainWindow::finishClose() {
    runner_.stop();presenter_.reset();
    if(m_hWnd) DestroyWindow();
    PostQuitMessage(0);
}

bool MainWindow::pumpOnce(DWORD timeout) {
    if (failure_) std::rethrow_exception(failure_);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        if(cheatDialog_ && cheatDialog_->m_hWnd && cheatDialog_->preTranslate(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (failure_) std::rethrow_exception(failure_);
    }
    if(cheatDialog_ && !cheatDialog_->m_hWnd) {cheatDialog_.reset();updateMenu();}
    const auto event = runner_.notificationEvent();
    const auto result = MsgWaitForMultipleObjectsEx(1, &event, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED) throw std::system_error(GetLastError(), std::system_category(), "MsgWaitForMultipleObjectsEx");
    if (result == WAIT_OBJECT_0) consumeSnapshot();
    return true;
}

int MainWindow::run(int showCommand) {
    ::ShowWindow(m_hWnd, showCommand);
    ::UpdateWindow(m_hWnd);
    if(!startupSettingsError_.empty()) showSettingsError(startupSettingsError_,m_hWnd);
    while (pumpOnce()) {}
    return 0;
}

LRESULT MainWindow::onCommandMessage(UINT, WPARAM wParam, LPARAM, BOOL&) {
    onCommand(LOWORD(wParam));
    return 0;
}

LRESULT MainWindow::onContextMenu(UINT, WPARAM, LPARAM position, BOOL& handled) {
    if (!fullscreenMenu_.m_hMenu) {
        handled = FALSE;
        return 0;
    }
    POINT anchor{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (position == -1) {
        RECT bounds{};
        requireWin32(::GetWindowRect(m_hWnd, &bounds), "Get fullscreen menu bounds");
        anchor = {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    }
    WTL::CMenu popup(::CreatePopupMenu());
    requireWin32(popup.m_hMenu != nullptr, "Create fullscreen menu");
    requireWin32(::AppendMenuW(popup.m_hMenu, MF_STRING, IDM_FULLSCREEN, L"E&xit fullscreen\tAlt+Enter"),
        "Add exit fullscreen command");
    fullscreenPopupOpen_ = true;
    refreshClientCursor();
    const auto selected = ::TrackPopupMenu(popup.m_hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        anchor.x, anchor.y, 0, m_hWnd, nullptr);
    fullscreenPopupOpen_ = false;
    if (selected == IDM_FULLSCREEN) onCommand(IDM_FULLSCREEN);
    else refreshClientCursor();
    return 0;
}

void MainWindow::refreshClientCursor() {
    POINT point{};
    if (::GetCursorPos(&point) && ::WindowFromPoint(point) == m_hWnd) {
        ::SendMessageW(m_hWnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(m_hWnd),
            MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}

LRESULT MainWindow::onSetCursor(UINT, WPARAM window, LPARAM position, BOOL& handled) {
    if (fullscreenMenu_.m_hMenu && inputActive_ && !fullscreenPopupOpen_ &&
        reinterpret_cast<HWND>(window) == m_hWnd && LOWORD(position) == HTCLIENT) {
        ::SetCursor(nullptr);
        return TRUE;
    }
    handled = FALSE;
    return 0;
}

LRESULT MainWindow::onKey(UINT message, WPARAM wParam, LPARAM lParam, BOOL& handled) {
    if(settingsOpen_ || (cheatDialog_ && cheatDialog_->m_hWnd)) return 0;
    const bool enter = wParam == VK_RETURN;
    const bool repeat = (lParam & (LPARAM{1} << 30)) != 0;
    const bool alt = (lParam & (LPARAM{1} << 29)) != 0;
    if (message == WM_SYSKEYDOWN) {
        if(wParam==VK_F9 && alt) {
            if(!repeat && !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_SHIFT)&0x8000)) onCommand(IDM_CHEAT_EDITOR);
            return 0;
        }
        handled = enter && alt;
        if (handled && !repeat) {
            fullscreenEnterDown_ = true;
            onCommand(IDM_FULLSCREEN);
        }
        return 0;
    }
    if (message == WM_SYSCHAR) {
        handled = wParam == L'\r' && alt;
        return 0;
    }
    if (message == WM_KEYDOWN) {
        if (enter && fullscreenEnterDown_) return 0;
        if(repeat) return 0;
        if(!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) &&
            !(GetKeyState(VK_SHIFT)&0x8000)) {
            if (wParam == VK_F7) {
                fpsVisible_ = !fpsVisible_;
                updateFps(true);
                render(true);
                return 0;
            }
            if(wParam==VK_F9) {onCommand(IDM_MEMORY_SEARCH);return 0;}
            if (wParam == VK_F8) {
                if (!fullscreenMenu_.m_hMenu) {
                    MONITORINFO monitor{sizeof(MONITORINFO)};
                    requireWin32(::GetMonitorInfoW(::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor),
                        "Get window scale monitor");
                    cycleWindowScale(monitor.rcWork);
                }
                return 0;
            }
            if(wParam>='0' && wParam<='9') {
                selectStateSlot(wParam=='0'?10:static_cast<unsigned>(wParam-'0'));
                return 0;
            }
            if((wParam==VK_F5 || wParam==VK_F6) && canOperate() && snapshot_.databaseRecord) {
                requestStateSlot(wParam==VK_F5);
                return 0;
            }
        }
        const auto action = input_.keyDown(static_cast<unsigned>(wParam), repeat);
        if (action == InputAction::TogglePause) onCommand(IDM_HOST_PAUSE);
        if (action == InputAction::ToggleFastForward) onCommand(IDM_FAST_FORWARD);
        sendInput(action == InputAction::ConsolePause);
    } else {
        input_.keyUp(static_cast<unsigned>(wParam));
        sendInput();
        if (enter && std::exchange(fullscreenEnterDown_, false)) return 0;
    }
    handled = FALSE; // Preserve default system-key processing after releasing input.
    return 0;
}

LRESULT MainWindow::onFocus(UINT message, WPARAM wParam, LPARAM, BOOL& handled) {
    inputActive_ = message == WM_SETFOCUS || (message == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE);
    if (!inputActive_) { fullscreenEnterDown_ = false; input_.clear(); runner_.clearInput(); }
    else if (message == WM_ACTIVATE) render(true);
    if (fullscreenMenu_.m_hMenu) refreshClientCursor();
    handled = FALSE;
    return 0;
}

void MainWindow::sendInput(bool consolePause) {
    runner_.setInput(input_.pad(),consolePause,input_.pad(1),input_.rapidButtons(0)|(input_.rapidButtons(1)<<2));
}

LRESULT MainWindow::onInputTimer(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
    if (wParam != inputTimerId) { handled = FALSE; return 0; }
    if(slotNoticeUntil_ && std::chrono::steady_clock::now()>=*slotNoticeUntil_) {
        slotNoticeUntil_.reset();
        presenter_->setSlotNotice(L"");
        render(true);
    }
    if(settingsOpen_ || (cheatDialog_ && cheatDialog_->m_hWnd)) return 0;
    InputAction action=InputAction::None;
    for(unsigned player=0;player<(hasSecondPad(snapshot_.machine)?2u:1u);++player) {
        const auto& pad=input_.configuration()[player];
        if(pad.source!=PadSource::XInput) continue;
        const auto current=input_.updateGamepad(player,gamepad_.poll(pad.slot),inputActive_);
        if(player==0) action=current;
    }
    switch(action) {
    case InputAction::TogglePause: onCommand(IDM_HOST_PAUSE); break;
    case InputAction::ToggleFastForward: onCommand(IDM_FAST_FORWARD); break;
    case InputAction::ToggleFullscreen: onCommand(IDM_FULLSCREEN); break;
    case InputAction::SaveState: requestStateSlot(true); break;
    case InputAction::LoadState: requestStateSlot(false); break;
    case InputAction::PreviousSlot:
        if(snapshot_.databaseRecord) selectStateSlot(activeStateSlot_==1?10:activeStateSlot_-1);
        break;
    case InputAction::NextSlot:
        if(snapshot_.databaseRecord) selectStateSlot(activeStateSlot_==10?1:activeStateSlot_+1);
        break;
    default: break;
    }
    sendInput(action == InputAction::ConsolePause);
    return 0;
}

LRESULT MainWindow::onSize(UINT, WPARAM wParam, LPARAM, BOOL&) {
    minimized_ = wParam == SIZE_MINIMIZED;
    if (presenter_) {
        RECT client{};
        requireWin32(::GetClientRect(m_hWnd, &client), "GetClientRect");
        presenter_->resize(minimized_ ? 0 : client.right, minimized_ ? 0 : client.bottom);
        render(true);
    }
    return 0;
}

LRESULT MainWindow::onDpiChanged(UINT, WPARAM, LPARAM lParam, BOOL&) {
    if (fullscreenMenu_.m_hMenu) return 0;
    const auto rect = reinterpret_cast<const RECT*>(lParam);
    requireWin32(::SetWindowPos(m_hWnd, nullptr, rect->left, rect->top, rect->right - rect->left,
        rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE), "Set DPI window rectangle");
    return 0;
}

LRESULT MainWindow::onPaint(UINT, WPARAM, LPARAM, BOOL&) {
    PAINTSTRUCT paint{};
    requireWin32(::BeginPaint(m_hWnd, &paint) != nullptr, "BeginPaint");
    ::EndPaint(m_hWnd, &paint);
    render(true);
    return 0;
}

LRESULT MainWindow::onClose(UINT, WPARAM, LPARAM, BOOL&) {
    close();
    return 0;
}

LRESULT MainWindow::onDestroy(UINT, WPARAM, LPARAM, BOOL&) {
    runner_.stop();
    presenter_.reset();
    ::PostQuitMessage(0);
    return 0;
}
} // namespace smsgg::win32
