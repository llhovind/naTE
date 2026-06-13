#pragma once

#include "session/SessionManager.h"

#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ui {

// Unified file transfer dialog. Source and destination are each independently
// chosen as either the local filesystem (SessionId 0) or any active SSH
// session. Replaces the old directional FileTransferDialog.
class TransferFilesDialog : public wxDialog {
public:
    // sessions: all active sessions eligible for file transfer (SupportsFileTransfer == true).
    // preSelectedSrcId: pre-selects this session as the source (0 = Local).
    TransferFilesDialog(
        wxWindow*                   parent,
        term::session::SessionManager& sm,
        std::vector<std::pair<term::session::SessionId, std::string>> sessions,
        term::session::SessionId    preSelectedSrcId = 0);

private:
    void OnAddSourceFiles(wxCommandEvent&);
    void OnBrowseSourceRemote(wxCommandEvent&);
    void OnRemoveSource(wxCommandEvent&);
    void OnSourceSessionChanged(wxCommandEvent&);
    void OnBrowseDest(wxCommandEvent&);
    void OnDestSessionChanged(wxCommandEvent&);
    void OnTransfer(wxCommandEvent&);
    void OnListSelectionChanged(wxCommandEvent&);

    term::session::SessionId SelectedSrcId() const;
    term::session::SessionId SelectedDstId() const;

    // Advances through fileList_; called after each file completes.
    void TransferNext();

    term::session::SessionManager& sm_;
    // Parallel arrays: index 0 = Local (id=0), indices 1..n = active sessions.
    std::vector<term::session::SessionId> sessionIds_;

    wxChoice*     srcChoice_    = nullptr;
    wxListBox*    fileList_     = nullptr;
    wxButton*     addBtn_       = nullptr;
    wxButton*     removeBtn_    = nullptr;
    wxChoice*     dstChoice_    = nullptr;
    wxTextCtrl*   dstDirCtrl_   = nullptr;
    wxButton*     browseDstBtn_ = nullptr;
    wxButton*     transferBtn_  = nullptr;
    wxStaticText* statusLabel_  = nullptr;

    int transferIndex_ = 0;
    int transferTotal_ = 0;
};

} // namespace ui
