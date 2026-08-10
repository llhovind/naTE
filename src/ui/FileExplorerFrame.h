#pragma once

#include "config/Config.h"
#include "fs/TransferQueue.h"
// Included rather than forward-declared: QueueOne takes the pane's nested
// Item type, which a forward declaration cannot reach.
#include "ui/FileExplorerPane.h"
#include "session/SessionManager.h"
#include "transport/LocalFileSystem.h"

#include <functional>
#include <memory>
#include <string>

#include <wx/button.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statusbr.h>

namespace ui {

class TransferPanel;

// Modeless per-session window: the local filesystem beside a remote one, with
// a transfer queue between them, riding the session's existing SSH connection.
//
// A coordinator, not a view. Each pane owns its own browsing and write
// operations; this class owns the things that only make sense *between* them —
// the transfer queue, the conflict policy, and the session's lifetime.
class FileExplorerFrame : public wxFrame, private term::fs::ITransferQueueListener {
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
    // does not vanish from under the user's cursor; the remote pane goes
    // offline and queued transfers are cancelled.
    // A session somewhere in the application has gone away. Panes showing it
    // go offline and its queued transfers are cancelled; anything pointing
    // elsewhere carries on, because a window can now span several sessions.
    void OnSessionEnded(term::session::SessionId id);

    // Explore gives the whole window to one listing; Transfer adds the second
    // pane, the copy buttons and the queue panel. Idempotent.
    void SetMode(FileExplorerMode mode);
    FileExplorerMode Mode() const noexcept { return mode_; }

    void ApplyConfig(const AppConfig& cfg);

    void SetOnClosed(std::function<void()> cb) { onClosed_ = std::move(cb); }

    // Reports size and sash position when they change, so the owner can save
    // them. Writing configuration is the owner's job.
    void SetOnGeometryChanged(
        std::function<void(int width, int height, int sash)> cb)
    {
        onGeometryChanged_ = std::move(cb);
    }

private:
    // --- ITransferQueueListener ----------------------------------------------
    void OnTransferJobAdded(term::fs::JobId id) override;
    void OnTransferJobChanged(term::fs::JobId id) override;
    void OnTransferQueueIdle() override;

    void BuildLayout(std::function<void(std::string)> onOpenInEditor);
    void UpdateTransferButtons();
    void RefreshEndpointChoices();

    // Every endpoint a pane may be pointed at, newest session state included.
    std::vector<PaneEndpoint> AvailableEndpoints() const;
    PaneEndpoint LocalEndpoint() const;
    PaneEndpoint EndpointForSession(term::session::SessionId id) const;

    // The single place that knows what each mode looks like.
    void ApplyMode();
    void UpdateQueueStatus();
    // Uploads files dropped from the desktop onto a pane. Dropping onto the
    // local pane is a no-op the user is told about rather than a silent one.
    void OnFilesDropped(FileExplorerPane* target, std::vector<std::string> paths);
    void PersistGeometry();
    void CopyBetweenPanes(FileExplorerPane* from, FileExplorerPane* to);
    // Queues one item, expanding directories recursively.
    void QueueOne(const FileExplorerPane::Item& item,
                  const term::fs::TransferEndpoint& source,
                  const term::fs::TransferEndpoint& destination,
                  const std::string& destinationDir);

    void OnDestroy(wxWindowDestroyEvent&);

    term::session::SessionId       sessionId_;
    term::session::SessionManager& sm_;
    AppConfig                      cfg_;
    std::function<void()>          onClosed_;
    // Invoked with the window's remembered geometry so the owner can persist
    // it. The frame does not write configuration itself.
    std::function<void(int width, int height, int sash)> onGeometryChanged_;

    std::unique_ptr<term::fs::TransferQueue> queue_;

    wxBoxSizer*       controls_    = nullptr;
    wxSplitterWindow* splitter_    = nullptr;
    FileExplorerPane* leftPane_    = nullptr;
    FileExplorerPane* rightPane_   = nullptr;
    TransferPanel*    transfers_   = nullptr;
    wxButton*         toRightBtn_  = nullptr;
    wxButton*         toLeftBtn_   = nullptr;
    wxButton*         modeBtn_     = nullptr;
    wxStatusBar*      status_      = nullptr;

    // The explorer opens in Explore mode; the second pane appears when the
    // user actually wants to compare or move something, which is the minority
    // of the time an admin has this window open.
    FileExplorerMode mode_ = FileExplorerMode::Explore;

    // The authority for where the split sits. Read back from the splitter
    // whenever the user moves it and before the split is torn down, so a
    // round trip through Explore mode cannot lose it.
    int  sashPosition_ = 0;          // 0 = let wx centre the split
    // Suppresses the sash events wx emits during a programmatic split or
    // unsplit, which would otherwise overwrite sashPosition_ with garbage.
    bool applyingMode_ = false;

    // Last text written to the queue status field, so a per-chunk progress
    // callback does not repaint the status bar on every SFTP block.
    wxString lastQueueStatus_;

    // True once a transfer has landed somewhere, so the idle notification
    // knows whether a refresh is worth the round trip.
    bool destinationDirty_ = false;
};

} // namespace ui
