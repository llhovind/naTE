#pragma once

#include "config/Config.h"
#include "fs/ExplorerController.h"
#include "session/SessionManager.h"

#include <functional>
#include <memory>
#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/frame.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>
#include <wx/textctrl.h>

namespace ui {

// Virtual list control backed by the controller's DirModel. Defined in the
// implementation file — the frame only ever holds a pointer to it.
class RemoteFileListCtrl;

// Modeless per-session window onto a remote filesystem, riding the session's
// existing SSH connection.
//
// A view only: navigation, history and listing state live in
// term::fs::ExplorerController, which is wx-free and unit-tested. This class
// translates wx events into controller calls and controller notifications into
// redraws.
//
// The list is virtual (wxLC_VIRTUAL): a directory can hold tens of thousands
// of entries, and inserting an item per row would make such a listing unusable.
class FileExplorerFrame : public wxFrame, private term::fs::IExplorerListener {
public:
    // onOpenInEditor receives the absolute remote path of a file the user
    // asked to edit; the caller routes it to the remote-edit workflow.
    FileExplorerFrame(wxWindow* parent,
                      term::session::SessionId sessionId,
                      term::session::SessionManager& sm,
                      const AppConfig& cfg,
                      std::string remoteDescription,
                      std::function<void(std::string)> onOpenInEditor);

    term::session::SessionId SessionId() const noexcept { return sessionId_; }

    // Called when the underlying session goes away. The window survives so it
    // does not vanish from under the user's cursor, but every operation that
    // would need the connection is disabled and the listing is cleared.
    void OnSessionEnded();

    void ApplyConfig(const AppConfig& cfg);

    // Invoked when the user closes the window, so the owner can forget it.
    void SetOnClosed(std::function<void()> cb) { onClosed_ = std::move(cb); }

private:
    // --- IExplorerListener ---------------------------------------------------
    void OnExplorerLoadingChanged(bool loading) override;
    void OnExplorerContentsChanged() override;
    void OnExplorerPathChanged(const std::string& path) override;

    // --- Event handlers ------------------------------------------------------
    void OnGo(wxCommandEvent&);
    void OnUp(wxCommandEvent&);
    void OnBack(wxCommandEvent&);
    void OnForward(wxCommandEvent&);
    void OnRefresh(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);
    void OnColumnClick(wxListEvent&);
    void OnContextMenu(wxListEvent&);
    void OnFilterChanged(wxCommandEvent&);
    void OnShowHiddenToggled(wxCommandEvent&);
    void OnDestroy(wxWindowDestroyEvent&);

    // --- Helpers -------------------------------------------------------------
    void BuildToolbar(wxWindow* parent, wxSizer* outer);
    void BuildList(wxSizer* outer);
    void RefreshRows();
    void UpdateNavigationState();
    void UpdateStatus();
    void CopyPathOf(size_t row);
    void ShowPropertiesFor(size_t row);
    void EditRow(size_t row);
    void ReportError(const wxString& what, const term::transport::FsError& err);

    term::session::SessionId       sessionId_;
    term::session::SessionManager& sm_;
    AppConfig                      cfg_;
    std::function<void(std::string)> onOpenInEditor_;
    std::function<void()>            onClosed_;

    // Null once the session has ended, which is also the guard on every
    // operation that would need the connection.
    std::unique_ptr<term::fs::ExplorerController> controller_;

    wxTextCtrl* pathCtrl_    = nullptr;
    wxTextCtrl* filterCtrl_  = nullptr;
    wxStaticText* filterLabel_ = nullptr;
    wxCheckBox* hiddenCheck_ = nullptr;
    wxButton*   backBtn_     = nullptr;
    wxButton*   forwardBtn_  = nullptr;
    wxButton*   upBtn_       = nullptr;
    wxButton*   refreshBtn_  = nullptr;
    RemoteFileListCtrl* list_ = nullptr;
    wxStatusBar* status_     = nullptr;
};

} // namespace ui
