#pragma once

#include "session/SessionManager.h"
#include "transport/Transport.hpp"

#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ui {

// Modal dialog that lets the user browse a remote SSH filesystem and select
// files for download. Calls SessionManager::ListRemoteDirectory via short-lived
// exec sessions. Returns selected remote paths via GetSelectedPaths().
class RemoteFileBrowserDialog : public wxDialog {
public:
    RemoteFileBrowserDialog(wxWindow* parent,
                            term::session::SessionId sessionId,
                            term::session::SessionManager& sm,
                            const std::string& remoteDescription,
                            const wxString& confirmLabel = "Add Selected");

    const std::vector<std::string>& GetSelectedPaths() const { return selectedPaths_; }

private:
    void Navigate(const std::string& path);
    void PopulateList(const std::vector<term::transport::RemoteDirEntry>& entries);
    std::string FullPath(const std::string& name) const;
    std::string ParentPath() const;

    void OnGo(wxCommandEvent&);
    void OnUp(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);
    void OnAdd(wxCommandEvent&);

    term::session::SessionId       sessionId_;
    term::session::SessionManager& sm_;

    wxTextCtrl* pathCtrl_  = nullptr;
    wxListCtrl* fileList_  = nullptr;
    wxButton*   upBtn_     = nullptr;
    wxButton*   addBtn_    = nullptr;
    wxStaticText* statusLabel_ = nullptr;

    std::string currentPath_;
    std::vector<term::transport::RemoteDirEntry> currentEntries_;
    std::vector<std::string> selectedPaths_;

    bool loading_ = false;
};

} // namespace ui
