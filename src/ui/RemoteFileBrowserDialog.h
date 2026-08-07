#pragma once

#include "session/SessionManager.h"
#include "transport/IRemoteFileSystem.h"

#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace ui {

enum class BrowseMode { Files, Directory };

// Modal dialog that lets the user browse a remote SSH filesystem. In Files
// mode, multi-selects individual files. In Directory mode, lets the user
// navigate and confirm a destination directory via "Select This Directory".
class RemoteFileBrowserDialog : public wxDialog {
public:
    RemoteFileBrowserDialog(wxWindow* parent,
                            term::session::SessionId sessionId,
                            term::session::SessionManager& sm,
                            const std::string& remoteDescription,
                            const wxString& confirmLabel = "Add Selected",
                            const std::string& initialPath = ".",
                            BrowseMode mode = BrowseMode::Files);

    const std::vector<std::string>& GetSelectedPaths() const { return selectedPaths_; }

private:
    void Navigate(const std::string& path);
    void PopulateList(const std::vector<term::transport::FileInfo>& entries);
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

    BrowseMode  mode_;
    std::string currentPath_;
    std::vector<term::transport::FileInfo> currentEntries_;
    std::vector<std::string> selectedPaths_;

    bool loading_ = false;
};

} // namespace ui
