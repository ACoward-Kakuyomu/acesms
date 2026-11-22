#include "win32/CheatDialog.h"
#include "win32/CheatExchange.h"

#include <algorithm>
#include <array>
#include <format>
#include <utility>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

namespace smsgg::win32 {
namespace {
constexpr std::array widths{SearchWidth::Byte,SearchWidth::Word,SearchWidth::Dword};
constexpr int searchControls[]{IDC_SEARCH_WIDTH_LABEL,IDC_SEARCH_WIDTH,IDC_SEARCH_CONDITION,IDC_SEARCH_VALUE,
    IDC_SEARCH_FILTER,IDC_SEARCH_NEW,IDC_SEARCH_RESULTS,IDC_SEARCH_HELP};
constexpr int cheatControls[]{IDC_CHEAT_LIST,IDC_CHEAT_NAME_LABEL,IDC_CHEAT_NAME,IDC_CHEAT_ADDRESS_LABEL,IDC_CHEAT_ADDRESS,
    IDC_CHEAT_WIDTH_LABEL,IDC_CHEAT_WIDTH,IDC_CHEAT_VALUE_LABEL,IDC_CHEAT_VALUE,IDC_CHEAT_ENABLED,IDC_CHEAT_ALWAYS,
    IDC_CHEAT_ADD,IDC_CHEAT_UPDATE,IDC_CHEAT_DELETE,IDC_CHEAT_APPLY,IDC_CHEAT_SAVE,IDC_CHEAT_IMPORT,IDC_CHEAT_EXPORT};

std::wstring controlText(HWND dialog,int id) {
    const auto control=GetDlgItem(dialog,id);
    const auto length=GetWindowTextLengthW(control);
    std::wstring value(length+1,L'\0');
    value.resize(GetWindowTextW(control,value.data(),length+1));
    return value;
}
std::optional<uint32_t> number(std::wstring_view text,SearchWidth width) {
    std::string ascii;
    ascii.reserve(text.size());
    for(const auto character:text) {
        if(character>0x7f) return {};
        ascii.push_back(static_cast<char>(character));
    }
    return parseSearchValue(ascii,width);
}
std::wstring hex(unsigned value,unsigned digits) {return std::format(L"0x{:0{}X}",value,digits);}
std::wstring displayValue(uint32_t value,SearchWidth width) {
    return std::to_wstring(value)+L" ("+hex(value,2*static_cast<unsigned>(width))+L")";
}
std::optional<SearchWidth> selectedWidth(HWND dialog,int id) {
    const auto selected=SendDlgItemMessageW(dialog,id,CB_GETCURSEL,0,0);
    if (selected < 0 || static_cast<size_t>(selected) >= widths.size()) return {};
    return widths[selected];
}
}

CheatDialog::CheatDialog(EmulatorRunner& runner,MemorySearch& search,Machine machine,RomIdentity identity,CheatSnapshot snapshot,bool editor)
    :runner_(runner),search_(search),machine_(machine),identity_(identity),wasPaused_(snapshot.wasPaused),editor_(editor),
     ram_(std::move(snapshot.ram)),saved_(std::move(snapshot.definitions)),draft_(saved_) {}

LRESULT CheatDialog::onInit(UINT,WPARAM,LPARAM,BOOL&) {
    const auto tabs=GetDlgItem(IDC_CHEAT_TABS);
    TCITEMW tab{TCIF_TEXT};
    wchar_t searchLabel[]=L"Search",cheatLabel[]=L"Cheats";
    tab.pszText=searchLabel;TabCtrl_InsertItem(tabs,0,&tab);
    tab.pszText=cheatLabel;TabCtrl_InsertItem(tabs,1,&tab);
    for(const auto label:{L"1 byte",L"2 bytes",L"4 bytes"}) {
        for(const auto id:{IDC_SEARCH_WIDTH,IDC_CHEAT_WIDTH}) SendDlgItemMessage(id,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    }
    SendDlgItemMessage(IDC_SEARCH_WIDTH,CB_SETCURSEL,std::ranges::find(widths,search_.width())-widths.begin(),0);
    SendDlgItemMessage(IDC_CHEAT_WIDTH,CB_SETCURSEL,0,0);
    for(const auto label:{L"Equals value",L"Unchanged",L"Changed",L"Increased",L"Decreased"}) {
        SendDlgItemMessage(IDC_SEARCH_CONDITION,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
    }
    SendDlgItemMessage(IDC_SEARCH_CONDITION,CB_SETCURSEL,0,0);
    SendDlgItemMessage(IDC_CHEAT_NAME,EM_SETLIMITTEXT,64,0);
    for(const auto id:{IDC_CHEAT_ADDRESS,IDC_SEARCH_VALUE,IDC_CHEAT_VALUE}) SendDlgItemMessage(id,EM_SETLIMITTEXT,16,0);
    const auto list=GetDlgItem(IDC_SEARCH_RESULTS);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT);
    const std::array<std::wstring,3> labels{L"Address",L"Current",L"Previous"};
    const std::array fractions{24,38,34};
    RECT client{};
    ::GetClientRect(list,&client);
    for(int column=0;column<3;++column) {
        LVCOLUMNW data{LVCF_TEXT|LVCF_WIDTH};
        data.pszText=const_cast<wchar_t*>(labels[column].c_str());
        data.cx=client.right*fractions[column]/100;
        ListView_InsertColumn(list,column,&data);
    }
    updateDefinitions();
    showPage(editor_);
    return TRUE;
}

bool CheatDialog::preTranslate(MSG& message) {
    if(!m_hWnd || (message.hwnd!=m_hWnd && !::IsChild(m_hWnd,message.hwnd))) return false;
    if(message.wParam==VK_F9 && (message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN)) {
        if(!(message.lParam&(LPARAM{1}<<30)) && pending_==RunnerOperation::None &&
            !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_SHIFT)&0x8000)) {
            if(message.message==WM_SYSKEYDOWN && (message.lParam&(LPARAM{1}<<29))) showPage(true);
            else if(message.message==WM_KEYDOWN) requestClose();
        }
        return true;
    }
    return ::IsDialogMessageW(m_hWnd,&message)!=FALSE;
}

void CheatDialog::status(std::wstring_view text) {
    SetDlgItemText(IDC_CHEAT_STATUS,std::wstring(text).c_str());
}

void CheatDialog::showPage(bool editor) {
    editor_=editor;
    TabCtrl_SetCurSel(GetDlgItem(IDC_CHEAT_TABS),editor?1:0);
    for(int id:searchControls) ::ShowWindow(GetDlgItem(id),editor?SW_HIDE:SW_SHOW);
    for(int id:cheatControls) ::ShowWindow(GetDlgItem(id),editor?SW_SHOW:SW_HIDE);
    if(!editor) {
        if(!captured_) {
            search_.capture(std::move(ram_));
            captured_=true;
        }
        updateSearchResults();
    } else status(L"");
    if(::IsWindowVisible(m_hWnd)) ::SetFocus(GetDlgItem(IDC_CHEAT_TABS));
}

void CheatDialog::updateSearchResults() {
    const auto list=GetDlgItem(IDC_SEARCH_RESULTS);
    ListView_SetItemCount(list,static_cast<int>(search_.size()));
    ::InvalidateRect(list,nullptr,FALSE);
    status(std::to_wstring(search_.size())+L" candidates");
}

void CheatDialog::updateDefinitions() {
    SendDlgItemMessage(IDC_CHEAT_LIST,LB_RESETCONTENT,0,0);
    for(const auto& entry:draft_) {
        const auto row=std::wstring(entry.enabled?L"[On] ":L"[Off] ")+entry.name+L"    "+
            hex(entry.address,4)+L"    "+displayValue(entry.value,entry.width)+(entry.always?L"    Always":L"");
        SendDlgItemMessage(IDC_CHEAT_LIST,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(row.c_str()));
    }
    if(selected_) SendDlgItemMessage(IDC_CHEAT_LIST,LB_SETCURSEL,*selected_,0);
    ::EnableWindow(GetDlgItem(IDC_CHEAT_UPDATE),selected_.has_value());
    ::EnableWindow(GetDlgItem(IDC_CHEAT_DELETE),selected_.has_value());
    ::EnableWindow(GetDlgItem(IDC_CHEAT_APPLY),selected_ && draft_[*selected_].enabled && !draft_[*selected_].always);
}

void CheatDialog::fillForm(const Cheat& definition) {
    updatingForm_=true;
    SetDlgItemText(IDC_CHEAT_NAME,definition.name.c_str());
    SetDlgItemText(IDC_CHEAT_ADDRESS,definition.address?hex(definition.address,4).c_str():L"");
    SetDlgItemText(IDC_CHEAT_VALUE,definition.address?std::to_wstring(definition.value).c_str():L"");
    SendDlgItemMessage(IDC_CHEAT_WIDTH,CB_SETCURSEL,std::ranges::find(widths,definition.width)-widths.begin(),0);
    CheckDlgButton(IDC_CHEAT_ENABLED,definition.enabled?BST_CHECKED:BST_UNCHECKED);
    CheckDlgButton(IDC_CHEAT_ALWAYS,definition.always?BST_CHECKED:BST_UNCHECKED);
    updatingForm_=false;
    formDirty_=false;
}

bool CheatDialog::storeForm(bool add) {
    const auto address=number(controlText(m_hWnd,IDC_CHEAT_ADDRESS),SearchWidth::Word);
    const auto width=selectedWidth(m_hWnd,IDC_CHEAT_WIDTH);
    if (!width) {
        status(L"Select a cheat width.");
        return false;
    }
    const auto value=number(controlText(m_hWnd,IDC_CHEAT_VALUE),*width);
    Cheat definition{controlText(m_hWnd,IDC_CHEAT_NAME),address.value_or(0),*width,value.value_or(0),
        IsDlgButtonChecked(IDC_CHEAT_ENABLED)==BST_CHECKED,IsDlgButtonChecked(IDC_CHEAT_ALWAYS)==BST_CHECKED};
    if(!value || !validCheat(machine_,definition)) {
        showPage(true);
        status(L"Enter a name and a value that fits in work RAM.");
        return false;
    }
    if(add) {
        selected_=draft_.size();
        draft_.push_back(std::move(definition));
    } else draft_[*selected_]=std::move(definition);
    formDirty_=false;
    updateDefinitions();
    status(L"");
    return true;
}

void CheatDialog::save(bool close) {
    if(formDirty_ && !storeForm(!selected_)) return;
    if(draft_==saved_) {if(close) finish();return;}
    if(runner_.saveCheats(draft_)) {
        setPending(RunnerOperation::SaveCheats);
        closeAfterSave_=close;
        status(L"Saving cheats...");
    } else status(L"Cannot save cheats while another operation is pending.");
}

void CheatDialog::setPending(RunnerOperation operation) {
    pending_=operation;
    const bool ready=operation==RunnerOperation::None;
    for(const auto id:cheatControls) ::EnableWindow(GetDlgItem(id),ready);
    ::EnableWindow(GetDlgItem(IDC_CHEAT_TABS),ready);
    ::EnableWindow(GetDlgItem(IDCANCEL),ready);
    if(ready) updateDefinitions();
}

void CheatDialog::exchange(bool exporting) {
    const auto dialogFailure=[&](HRESULT result) {
        status(std::format(L"File selection failed (HRESULT 0x{:08X}).",static_cast<unsigned long>(result)));
    };
    Microsoft::WRL::ComPtr<IFileDialog> picker;
    auto result=::CoCreateInstance(exporting?CLSID_FileSaveDialog:CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&picker));
    if(FAILED(result)) {dialogFailure(result);return;}
    constexpr COMDLG_FILTERSPEC filters[]{{L"Cheat JSON",L"*.json"}};
    DWORD options{};
    if(SUCCEEDED(result=picker->GetOptions(&options)))
        result=picker->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|(exporting?FOS_OVERWRITEPROMPT:FOS_FILEMUSTEXIST));
    if(SUCCEEDED(result)) result=picker->SetFileTypes(1,filters);
    if(SUCCEEDED(result)) result=picker->SetDefaultExtension(L"json");
    if(SUCCEEDED(result) && exporting) result=picker->SetFileName(L"cheats.json");
    if(SUCCEEDED(result)) result=picker->SetTitle(exporting?L"Export Cheats":L"Import Cheats");
    if(SUCCEEDED(result)) result=picker->Show(m_hWnd);
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    Microsoft::WRL::ComPtr<IShellItem> item;
    if(SUCCEEDED(result)) result=picker->GetResult(&item);
    PWSTR raw{};
    if(SUCCEEDED(result)) result=item->GetDisplayName(SIGDN_FILESYSPATH,&raw);
    if(FAILED(result)) {dialogFailure(result);return;}
    const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> selected(raw,&CoTaskMemFree);
    const std::filesystem::path path(selected.get());
    const auto report=[&](const CheatExchangeError& error) {
        const auto message=path.wstring()+L"\n"+error.message;
        ::MessageBoxW(m_hWnd,message.c_str(),exporting?L"Export Cheats":L"Import Cheats",MB_OK|MB_ICONERROR);
    };
    if(exporting) {
        if(const auto error=writeCheatExchange(path,identity_,machine_,saved_)) report(*error);
        else status(L"Saved cheat definitions exported.");
        return;
    }
    auto loaded=readCheatExchange(path,identity_,machine_,draft_);
    if(const auto* error=std::get_if<CheatExchangeError>(&loaded)) {report(*error);return;}
    auto& merged=std::get<std::vector<Cheat>>(loaded);
    const auto added=merged.size()-draft_.size();
    draft_=std::move(merged);
    updateDefinitions();
    status(added?std::to_wstring(added)+L" imported (disabled). Choose Save Cheats to keep them.":L"No new cheat definitions.");
}

void CheatDialog::finish() {
    const auto owner=::GetWindow(m_hWnd,GW_OWNER);
    ::EnableWindow(owner,TRUE);
    DestroyWindow();
    ::SetActiveWindow(owner);
    runner_.setPaused(wasPaused_);
}

void CheatDialog::requestClose() {
    if(pending_!=RunnerOperation::None) return;
    if(formDirty_ || draft_!=saved_) {
        const auto choice=::MessageBoxW(m_hWnd,L"Save your changes to the cheat list?",L"Unsaved cheats",MB_YESNOCANCEL|MB_ICONQUESTION);
        if(choice==IDYES) {save(true);return;}
        if(choice!=IDNO) return;
    }
    finish();
}

void CheatDialog::onResult(const RunnerSnapshot& snapshot,std::wstring_view error) {
    if(snapshot.operation!=pending_ || pending_==RunnerOperation::None) return;
    const auto completed=pending_;
    setPending(RunnerOperation::None);
    const auto close=std::exchange(closeAfterSave_,false);
    if(!snapshot.operationSucceeded) {status(error);return;}
    if(completed==RunnerOperation::SaveCheats) {
        saved_=draft_;
        if(close) {finish();return;}
        status(L"Cheats saved.");
    } else if(snapshot.cheatSnapshot) {
        if(captured_) search_.refresh(snapshot.cheatSnapshot->ram);
        else ram_=snapshot.cheatSnapshot->ram;
        status(L"Cheat applied.");
    }
}

LRESULT CheatDialog::onCommand(UINT,WPARAM command,LPARAM,BOOL&) {
    const auto id=LOWORD(command), event=HIWORD(command);
    if(pending_!=RunnerOperation::None) return 0;
    if(id==IDCANCEL) requestClose();
    else if(id==IDOK && editor_) {if(formDirty_) storeForm(!selected_);}
    else if(id==IDC_SEARCH_NEW || (id==IDC_SEARCH_WIDTH && event==CBN_SELCHANGE)) {
        const auto width = selectedWidth(m_hWnd, IDC_SEARCH_WIDTH);
        if (!width) {
            status(L"Select a search width.");
            return 0;
        }
        search_.reset(*width);
        updateSearchResults();
    } else if(id==IDC_SEARCH_CONDITION && event==CBN_SELCHANGE) {
        ::EnableWindow(GetDlgItem(IDC_SEARCH_VALUE),SendDlgItemMessage(IDC_SEARCH_CONDITION,CB_GETCURSEL,0,0)==0);
    } else if(id==IDC_SEARCH_FILTER || id==IDOK) {
        const auto selected = SendDlgItemMessage(IDC_SEARCH_CONDITION, CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected > static_cast<int>(SearchCondition::Decreased)) {
            status(L"Select a search condition.");
            return 0;
        }
        const auto condition=static_cast<SearchCondition>(selected);
        const auto value=condition==SearchCondition::Equal?number(controlText(m_hWnd,IDC_SEARCH_VALUE),search_.width()):std::optional<uint32_t>{0};
        if(!value) status(L"Enter a value that fits the selected width.");
        else if(!search_.filter(condition,*value)) status(L"Resume and capture again before comparing.");
        else updateSearchResults();
    } else if(id==IDC_CHEAT_LIST && event==LBN_SELCHANGE) {
        const auto index=SendDlgItemMessage(IDC_CHEAT_LIST,LB_GETCURSEL,0,0);
        if(index!=LB_ERR) {
            if(formDirty_ && ::MessageBoxW(m_hWnd,L"Discard changes to the current entry?",L"Unsaved entry",MB_YESNO|MB_ICONQUESTION)!=IDYES) {
                SendDlgItemMessage(IDC_CHEAT_LIST,LB_SETCURSEL,selected_?static_cast<WPARAM>(*selected_):static_cast<WPARAM>(-1),0);
                return 0;
            }
            selected_=static_cast<size_t>(index);
            fillForm(draft_[*selected_]);
            updateDefinitions();
        }
    } else if(id==IDC_CHEAT_ADD) storeForm(true);
    else if(id==IDC_CHEAT_UPDATE && selected_) storeForm(false);
    else if(id==IDC_CHEAT_DELETE && selected_) {
        draft_.erase(draft_.begin()+*selected_);
        selected_.reset();
        fillForm({});
        updateDefinitions();
    } else if(id==IDC_CHEAT_SAVE) save(false);
    else if(id==IDC_CHEAT_IMPORT || id==IDC_CHEAT_EXPORT) exchange(id==IDC_CHEAT_EXPORT);
    else if(id==IDC_CHEAT_APPLY && selected_) {
        if(formDirty_) status(L"Add or update the entry before applying.");
        else if(runner_.applyCheat(draft_[*selected_])) {
            setPending(RunnerOperation::ApplyCheat);
            status(L"Applying cheat...");
        }
    } else if(!updatingForm_ && ((event==EN_CHANGE && (id==IDC_CHEAT_NAME || id==IDC_CHEAT_ADDRESS || id==IDC_CHEAT_VALUE)) ||
        (event==CBN_SELCHANGE && id==IDC_CHEAT_WIDTH) || (event==BN_CLICKED && (id==IDC_CHEAT_ENABLED || id==IDC_CHEAT_ALWAYS)))) {
        formDirty_=true;
    }
    return 0;
}

LRESULT CheatDialog::onNotify(UINT,WPARAM,LPARAM data,BOOL&) {
    const auto* notification=reinterpret_cast<const NMHDR*>(data);
    if(notification->idFrom==IDC_CHEAT_TABS && notification->code==TCN_SELCHANGE && pending_==RunnerOperation::None) {
        showPage(TabCtrl_GetCurSel(GetDlgItem(IDC_CHEAT_TABS))==1);
    } else if(notification->idFrom==IDC_SEARCH_RESULTS && notification->code==LVN_GETDISPINFOW) {
        auto& item=reinterpret_cast<NMLVDISPINFOW*>(data)->item;
        if((item.mask&LVIF_TEXT) && item.iItem>=0 && static_cast<size_t>(item.iItem)<search_.size()) {
            const auto candidate=search_.result(item.iItem);
            std::wstring text;
            if(item.iSubItem==0) text=hex(candidate.address,4);
            else if(item.iSubItem==1) text=displayValue(candidate.current,search_.width());
            else if(candidate.previous) text=displayValue(*candidate.previous,search_.width());
            lstrcpynW(item.pszText,text.c_str(),item.cchTextMax);
        }
    } else if(notification->idFrom==IDC_SEARCH_RESULTS && notification->code==NM_DBLCLK && pending_==RunnerOperation::None) {
        const auto index=reinterpret_cast<const NMITEMACTIVATE*>(data)->iItem;
        if(index<0 || static_cast<size_t>(index)>=search_.size()) return 0;
        if(formDirty_ && ::MessageBoxW(m_hWnd,L"Discard changes to the current entry?",L"Unsaved entry",MB_YESNO|MB_ICONQUESTION)!=IDYES) return 0;
        const auto candidate=search_.result(index);
        selected_.reset();
        fillForm({L"RAM "+hex(candidate.address,4),candidate.address,search_.width(),candidate.current});
        formDirty_=true;
        updateDefinitions();
        showPage(true);
        ::SetFocus(GetDlgItem(IDC_CHEAT_NAME));
    }
    return 0;
}

LRESULT CheatDialog::onClose(UINT,WPARAM,LPARAM,BOOL&) {
    requestClose();
    return 0;
}

} // namespace smsgg::win32
