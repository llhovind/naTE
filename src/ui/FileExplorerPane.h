#pragma once

#include "config/Config.h"
#include "fs/ExplorerController.h"
#include "fs/RemoteDeleter.h"
#include "session/SessionManager.h"
#include "transport/IRemoteFileSystem.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "ui/RemoteFileListCtrl.h"

namespace ui {

// A filesystem a pane can be pointed at: this computer, or any session that
// has one. sessionId is 0 for the local machine, which is also how a pane
// reports which session it is showing when one dies.
struct PaneEndpoint {
    std::string                         label;
    term::transport::IRemoteFileSystem* fs = nullptr;
    term::session::SessionId            sessionId = 0;
    // Where to open when the pane binds to this endpoint.
    std::string                         defaultPath;

    bool Valid() const noexcept { return fs != nullptr; }
    bool IsLocalDisk() const noexcept { return fs && fs->IsLocalDisk(); }
};

// One browsable filesystem: toolbar, listing, filter and the write operations
// that act on it.
//
// Deliberately agnostic about *which* filesystem. The local and remote sides
// of the explorer are two instances of this class over two IRemoteFileSystem
// implementations, which is the whole reason the local adapter exists — the
// alternative was a second, drifting copy of browsing and sorting.
class FileExplorerPane : public wxPanel, private term::fs::IExplorerListener {
public:
    // One row the user has selected, carrying everything an operation needs
    // without the caller reaching back into the model.
    struct Item {
        std::string path;
        std::string name;
        bool        isDir     = false;
        bool        isSymlink = false;
        uint64_t    size      = 0;
    };

    // The provider is queried each time the endpoint list is refreshed rather
    // than captured once: sessions open and close while this window is up, and
    // a cached list would offer the user a dead one.
    using EndpointProvider = std::function<std::vector<PaneEndpoint>()>;

    FileExplorerPane(wxWindow* parent,
                     const AppConfig& cfg,
                     EndpointProvider provider,
                     PaneEndpoint initial);

    // Points the pane at a different filesystem, discarding the current
    // listing and history.
    void BindEndpoint(const PaneEndpoint& endpoint);
    const PaneEndpoint& CurrentEndpoint() const noexcept { return endpoint_; }

    // Re-reads the endpoint list and rebuilds the selector, preserving the
    // current selection when it is still available.
    void RefreshEndpointChoices();

    // Fired when the user picks a different endpoint from the selector.
    void SetOnEndpointChanged(std::function<void()> cb)
    {
        onEndpointChanged_ = std::move(cb);
    }

    // Invoked with the endpoint the path belongs to and the path itself. The
    // pane reports its own endpoint rather than letting the owner assume one:
    // both panes can be pointed at any filesystem, so a path is meaningless
    // without the machine it came from. sessionId is 0 for this computer.
    void SetOnOpenInEditor(std::function<void(term::session::SessionId, std::string)> cb)
    {
        onOpenInEditor_ = std::move(cb);
    }
    // Fired when the selection or the current directory changes, so the owner
    // can re-evaluate what the transfer buttons should say.
    void SetOnStateChanged(std::function<void()> cb) { onStateChanged_ = std::move(cb); }

    // Invoked with local paths dropped onto this pane from the desktop. The
    // pane cannot queue transfers itself — it has one endpoint, and a transfer
    // needs two — so the owner decides what to do with them.
    void SetOnLocalFilesDropped(std::function<void(std::vector<std::string>)> cb)
    {
        onLocalFilesDropped_ = std::move(cb);
    }

    // Moves keyboard focus onto the listing, for shortcuts owned by the frame.
    void FocusList();

    const std::string& CurrentPath() const;
    std::vector<Item>  SelectedItems() const;
    bool               IsLive() const noexcept { return controller_ != nullptr; }

    void Refresh();
    // Re-reads and then puts the cursor on focusName if it is present.
    void RefreshFocusing(std::string focusName);

    // Drops the controller: every operation becomes a no-op and the listing is
    // cleared. Used when the session behind a remote pane goes away.
    void GoOffline(const wxString& reason);

    void ApplyConfig(const AppConfig& cfg);

private:
    // --- IExplorerListener ---------------------------------------------------
    void OnExplorerLoadingChanged(bool loading) override;
    void OnExplorerContentsChanged() override;
    void OnExplorerPathChanged(const std::string& path) override;

    // --- Events --------------------------------------------------------------
    void OnGo(wxCommandEvent&);
    void OnItemActivated(wxListEvent&);
    void OnColumnClick(wxListEvent&);
    void OnContextMenu(wxListEvent&);
    void OnListKeyDown(wxListEvent&);
    void OnSelectionChanged(wxListEvent&);

    // --- Construction --------------------------------------------------------
    void BuildToolbar(wxSizer* outer);
    void OnEndpointSelected(wxCommandEvent&);
    void BuildList(wxSizer* outer);

    // --- View ----------------------------------------------------------------
    void RefreshRows();
    void UpdateNavigationState();
    void UpdateStatus();
    void SetStatus(const wxString& text);

    // --- Operations ----------------------------------------------------------
    // No row. Returned when a name no longer appears in the listing.
    static constexpr size_t kNoRow = static_cast<size_t>(-1);

    // Re-resolves a row that was identified before a nested event loop ran.
    //
    // A context menu and a modal dialog both pump events, so a listing can be
    // rebuilt between the user picking a row and the action running. An index
    // captured beforehand would then point at whatever row happens to sit there
    // now; the name is what the user actually chose.
    size_t RowForName(const std::string& name) const;

    void CopyPathOf(size_t row);
    void ShowPropertiesFor(size_t row);
    void EditRow(size_t row);
    void NewFolder();
    void RenameRow(size_t row);
    void DeleteSelection();
    bool ConfirmDeletion(const term::fs::DeletePlan& plan, const wxString& target);
    void AfterWrite(const wxString& what, const term::transport::FsError& err,
                    std::string focusName);
    bool RequireLive();

    EndpointProvider                    provider_;
    PaneEndpoint                        endpoint_;
    std::vector<PaneEndpoint>           choices_;
    AppConfig                           cfg_;
    std::function<void(term::session::SessionId, std::string)> onOpenInEditor_;
    std::function<void()>               onStateChanged_;
    std::function<void()>               onEndpointChanged_;
    std::function<void(std::vector<std::string>)> onLocalFilesDropped_;

    std::unique_ptr<term::fs::ExplorerController> controller_;
    std::unique_ptr<term::fs::RemoteDeleter>      deleter_;
    term::fs::DispatchGuard                       guard_;

    std::string pendingFocusName_;

    wxChoice*     endpointChoice_ = nullptr;
    wxTextCtrl*   pathCtrl_    = nullptr;
    wxTextCtrl*   filterCtrl_  = nullptr;
    wxStaticText* filterLabel_ = nullptr;
    wxCheckBox*   hiddenCheck_ = nullptr;
    wxButton*     backBtn_     = nullptr;
    wxButton*     forwardBtn_  = nullptr;
    wxButton*     upBtn_       = nullptr;
    wxButton*     refreshBtn_  = nullptr;
    wxButton*     newFolderBtn_ = nullptr;
    RemoteFileListCtrl* list_  = nullptr;
    wxStaticText* status_      = nullptr;
};

} // namespace ui
