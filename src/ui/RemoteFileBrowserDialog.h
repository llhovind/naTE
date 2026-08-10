#pragma once

#include "fs/ExplorerController.h"
#include "session/SessionManager.h"
#include "ui/RemoteFileListCtrl.h"

#include <memory>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ui {

// Modal picker for a single remote file.
//
// A thin view over the same ExplorerController and DirModel the file explorer
// uses, so navigation, canonicalisation, symlink handling and sorting have one
// implementation rather than a full one and a lesser copy that drifts from it.
// What remains here is only what makes this a *picker*: modality, a confirm
// button, and the chosen path.
//
// Single-selection on purpose. Its one caller opens one file in an editor, and
// a multi-select list that silently discarded all but the first choice would
// be advertising something it does not do.
class RemoteFileBrowserDialog : public wxDialog,
                                private term::fs::IExplorerListener {
public:
    RemoteFileBrowserDialog(wxWindow* parent,
                            term::session::SessionId sessionId,
                            term::session::SessionManager& sm,
                            const std::string& remoteDescription,
                            const wxString& confirmLabel = "Select",
                            const std::string& initialPath = ".");

    // Empty unless the dialog closed with wxID_OK.
    const std::string& GetSelectedPath() const { return selectedPath_; }

private:
    // --- IExplorerListener ---------------------------------------------------
    void OnExplorerLoadingChanged(bool loading) override;
    void OnExplorerContentsChanged() override;
    void OnExplorerPathChanged(const std::string& path) override;

    void OnGo(wxCommandEvent&);
    void OnUp(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);
    void OnConfirm(wxCommandEvent&);

    void UpdateControls();

    term::session::SessionManager& sm_;

    std::unique_ptr<term::fs::ExplorerController> controller_;
    std::string                                   selectedPath_;

    wxTextCtrl*         pathCtrl_    = nullptr;
    RemoteFileListCtrl* fileList_    = nullptr;
    wxButton*           upBtn_       = nullptr;
    wxButton*           confirmBtn_  = nullptr;
    wxStaticText*       statusLabel_ = nullptr;
};

} // namespace ui
