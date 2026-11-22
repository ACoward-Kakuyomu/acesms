#pragma once

#include "win32/EmulatorRunner.h"
#include "win32/WtlApplication.h"
#include "win32/Resource.h"

#include <atlwin.h>
#include <commctrl.h>

namespace smsgg::win32 {

class CheatDialog final : public ATL::CDialogImpl<CheatDialog> {
public:
    enum { IDD=IDD_CHEATS };
    CheatDialog(EmulatorRunner& runner,MemorySearch& search,Machine machine,RomIdentity identity,CheatSnapshot snapshot,bool editor);
    bool preTranslate(MSG& message);
    void onResult(const RunnerSnapshot& snapshot,std::wstring_view error);
    void requestClose();

    BEGIN_MSG_MAP(CheatDialog)
        MESSAGE_HANDLER(WM_INITDIALOG,onInit)
        MESSAGE_HANDLER(WM_COMMAND,onCommand)
        MESSAGE_HANDLER(WM_NOTIFY,onNotify)
        MESSAGE_HANDLER(WM_CLOSE,onClose)
    END_MSG_MAP()

private:
    LRESULT onInit(UINT,WPARAM,LPARAM,BOOL&);
    LRESULT onCommand(UINT,WPARAM,LPARAM,BOOL&);
    LRESULT onNotify(UINT,WPARAM,LPARAM,BOOL&);
    LRESULT onClose(UINT,WPARAM,LPARAM,BOOL&);
    void showPage(bool editor);
    void updateSearchResults();
    void updateDefinitions();
    void fillForm(const Cheat& definition);
    bool storeForm(bool add);
    void save(bool close);
    void exchange(bool exporting);
    void setPending(RunnerOperation operation);
    void finish();
    void status(std::wstring_view text);

    EmulatorRunner& runner_;
    MemorySearch& search_;
    const Machine machine_;
    const RomIdentity identity_;
    const bool wasPaused_;
    bool editor_,captured_{},updatingForm_{},formDirty_{},closeAfterSave_{};
    std::optional<size_t> selected_;
    std::vector<uint8_t> ram_;
    std::vector<Cheat> saved_,draft_;
    RunnerOperation pending_{RunnerOperation::None};
};

} // namespace smsgg::win32
