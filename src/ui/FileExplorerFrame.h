#pragma once

#include "config/Config.h"
#include "fs/TransferQueue.h"
// Included rather than forward-declared: QueueOne takes the pane's nested
// Item type, which a forward declaration cannot reach.
#include "ui/FileExplorerPane.h"
#include "ui/PaneGeometry.h"
#include "session/SessionManager.h"
#include "transport/LocalFileSystem.h"

#include <functional>
#include <memory>
#include <string>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/frame.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/statusbr.h>

namespace ui {

class TransferPanel;

// Reports a file the user asked to edit: the endpoint it lives on (0 for this
// computer) and its absolute path on that endpoint.
using OpenInEditorFn = std::function<void(term::session::SessionId, std::string)>;

// Modeless per-session window: the local filesystem beside a remote one, with
// a transfer queue between them, riding the session's existing SSH connection.
//
// A coordinator, not a view. Each pane owns its own browsing and write
// operations; this class owns the things that only make sense *between* them —
// the transfer queue, the conflict policy, and the session's lifetime.
class FileExplorerFrame : public wxFrame, private term::fs::ITransferQueueListener {
public:
    // onOpenInEditor receives the endpoint a file lives on and its absolute
    // path; the caller routes it to the edit workflow. The window does not
    // supply the endpoint itself — either pane may be pointed anywhere,
    // including at this computer, so only the pane knows.
    FileExplorerFrame(wxWindow* parent,
                      term::session::SessionId sessionId,
                      term::session::SessionManager& sm,
                      const AppConfig& cfg,
                      OpenInEditorFn onOpenInEditor);

    term::session::SessionId SessionId() const noexcept { return sessionId_; }

    // Called when the underlying session goes away. The window survives so it
    // does not vanish from under the user's cursor; the remote pane goes
    // offline and queued transfers are cancelled.
    // A session somewhere in the application has gone away. Panes showing it
    // go offline and its queued transfers are cancelled; anything pointing
    // elsewhere carries on, because a window can now span several sessions.
    void OnSessionEnded(term::session::SessionId id);

    // A file on some endpoint has been written from outside this window — an
    // external editor saving back, for instance. Any pane sitting in that
    // file's directory re-reads it; everything else is untouched, so a save
    // costs at most one listing per pane that was actually looking at it.
    void OnFileChanged(term::session::SessionId endpoint, const std::string& path);

    // Explore gives the whole window to one listing; Transfer adds the second
    // pane, the copy buttons and the queue panel. Idempotent.
    void SetMode(FileExplorerMode mode);
    FileExplorerMode Mode() const noexcept { return mode_; }

    void ApplyConfig(const AppConfig& cfg);

    void SetOnClosed(std::function<void()> cb) { onClosed_ = std::move(cb); }

    // Repoints (or, with nullptr, detaches) the edit callback on both panes.
    // The owner detaches during teardown: wx may destroy this window after the
    // manager that the callback reaches back into has gone.
    void SetOnOpenInEditor(OpenInEditorFn cb);

    // Reports the window's remembered appearance when it changes, so the owner
    // can save it. The width reported is always the *one-pane* width, whatever
    // mode is showing — see the geometry section below. Writing configuration
    // is the owner's job.
    void SetOnLayoutChanged(std::function<void(const FileExplorerLayout&)> cb)
    {
        onLayoutChanged_ = std::move(cb);
    }

    // Places the window before it is first shown. Separate from construction
    // because *where* a new window goes is the owner's decision: it is the one
    // that knows how many explorer windows are already open, and so whether
    // this one would land exactly on top of another.
    void PlaceAt(const wxPoint& position) { Move(position); }

private:
    // --- ITransferQueueListener ----------------------------------------------
    void OnTransferJobAdded(term::fs::JobId id) override;
    void OnTransferJobChanged(term::fs::JobId id) override;
    void OnTransferQueueIdle() override;

    void BuildLayout(OpenInEditorFn onOpenInEditor);
    void UpdateTransferButtons();
    // Names the endpoints currently on show, so the window is identifiable
    // from a taskbar. Pulled from live pane state; safe to call at any time.
    void UpdateTitle();
    void RefreshEndpointChoices();

    // Every endpoint a pane may be pointed at, newest session state included.
    std::vector<PaneEndpoint> AvailableEndpoints() const;
    PaneEndpoint LocalEndpoint() const;
    PaneEndpoint EndpointForSession(term::session::SessionId id) const;

    // The single place that knows what each mode looks like.
    void ApplyMode();

    // --- Geometry -------------------------------------------------------------
    // The window is measured in *panes*: one pane wide in Explore mode, two
    // equal panes wide in Transfer mode. Every figure is derived from what is
    // on screen right now rather than from a remembered number, so switching
    // mode is a pure widen/narrow and a resize survives the round trip.
    PaneMetrics Metrics() const;                // live measurements for the policy
    int  FrameChromeWidth() const;              // border + sizer margins
    int  PaneWidth() const;                     // width of the leading pane
    int  FrameWidthForPanes(int panes) const;
    int  MinFrameWidthForPanes(int panes) const;
    // Narrowest the frame may be and still show every control on the row. Only
    // meaningful once the row is laid out in its final state for the mode.
    int  ControlsRowMinWidth() const;
    void RestoreListingHeight(int wanted);      // undo what the queue panel cost
    void SetFrameSize(int width, int height);
    void CentreSash();
    // Lands the seam between the two copy buttons on the sash, so each button
    // sits over the pane it copies out of. The overload taking a position is
    // for the events that carry a sash wx has not applied yet; the other reads
    // the sash where it stands.
    void AlignCopyButtonsToSash();
    void AlignCopyButtonsToSash(int sashPosition);
    void UpdateQueueStatus();
    // Uploads files dropped from the desktop onto a pane. Dropping onto the
    // local pane is a no-op the user is told about rather than a silent one.
    void OnFilesDropped(FileExplorerPane* target, std::vector<std::string> paths);
    // Keeps the two listings' columns in step, then saves. `source` is the pane
    // whose divider the user dragged.
    void OnColumnWidthsChanged(FileExplorerPane* source);
    void PersistLayout();
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
    // Invoked with the window's remembered appearance so the owner can persist
    // it. The frame does not write configuration itself.
    std::function<void(const FileExplorerLayout&)> onLayoutChanged_;

    std::unique_ptr<term::fs::TransferQueue> queue_;

    wxBoxSizer*       controls_    = nullptr;
    // The spacer that positions the copy pair against the sash. Owned by
    // controls_; held only to resize it.
    wxSizerItem*      copyPairGap_ = nullptr;
    wxSplitterWindow* splitter_    = nullptr;
    FileExplorerPane* leftPane_    = nullptr;
    FileExplorerPane* rightPane_   = nullptr;
    TransferPanel*    transfers_   = nullptr;
    wxButton*         toRightBtn_  = nullptr;
    wxButton*         toLeftBtn_   = nullptr;
    wxButton*         modeBtn_     = nullptr;
    // How copies treat symlinks. Lives on the frame rather than a pane because
    // it governs the queue, which the frame owns, exactly as the conflict
    // policy does — and because it applies to a transfer, not to browsing.
    // The label is held because it shares the dropdown's visibility and its
    // width counts towards the room the copy buttons have to move in.
    wxStaticText*     symlinkLabel_  = nullptr;
    wxChoice*         symlinkChoice_ = nullptr;
    wxStatusBar*      status_      = nullptr;

    // The explorer opens in Explore mode; the second pane appears when the
    // user actually wants to compare or move something, which is the minority
    // of the time an admin has this window open.
    FileExplorerMode mode_ = FileExplorerMode::Explore;

    // Suppresses the sash events wx emits during a programmatic split, unsplit
    // or resize, which would otherwise persist a shape mid-reshape.
    bool applyingMode_ = false;

    // How much taller the frame is in Transfer mode, learned by measuring the
    // last switch rather than assumed. Explore mode subtracts it to describe
    // the shape a window should reopen at. 0 until the first switch, which is
    // correct: a window that has never shown the queue owes nothing for it.
    int transferHeightDelta_ = 0;

    // Last text written to the queue status field, so a per-chunk progress
    // callback does not repaint the status bar on every SFTP block.
    wxString lastQueueStatus_;

    // True once a transfer has landed somewhere, so the idle notification
    // knows whether a refresh is worth the round trip.
    bool destinationDirty_ = false;
};

} // namespace ui
