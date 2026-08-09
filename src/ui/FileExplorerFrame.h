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

    void ApplyConfig(const AppConfig& cfg);

    void SetOnClosed(std::function<void()> cb) { onClosed_ = std::move(cb); }

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

    void ShowSecondPane(bool show);
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

    std::unique_ptr<term::fs::TransferQueue> queue_;

    wxSplitterWindow* splitter_    = nullptr;
    FileExplorerPane* leftPane_    = nullptr;
    FileExplorerPane* rightPane_   = nullptr;
    TransferPanel*    transfers_   = nullptr;
    wxButton*         toRightBtn_  = nullptr;
    wxButton*         toLeftBtn_   = nullptr;
    wxButton*         splitBtn_    = nullptr;
    wxStatusBar*      status_      = nullptr;

    // The explorer opens as a single pane; the second appears when the user
    // actually wants to compare or move something, which is the minority of
    // the time an admin has this window open.
    bool secondPaneShown_ = false;

    // True once a transfer has landed somewhere, so the idle notification
    // knows whether a refresh is worth the round trip.
    bool destinationDirty_ = false;
};

} // namespace ui
