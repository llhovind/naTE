#pragma once
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <wx/panel.h>
#include "config/Config.h"
#include "session/ISessionObserver.h"
#include "ui/ISessionDropTarget.h"
#include "ui/TerminalActions.h"
#include "ui/TileActions.h"
#include "ui/WrapControl.h"

class TerminalPanel;
class TabStrip;

// TerminalTile is a tab container: one title bar shared across N sessions,
// with only the active session's TerminalPanel visible at a time.
// UIManager creates TerminalPanel instances externally and registers them
// via AddTab(); the tile takes wx-ownership (panels are parented to contentArea_).
class TerminalTile : public wxPanel, public ui::ISessionDropTarget {
public:
    explicit TerminalTile(wxWindow* parent, const AppConfig& cfg);

    // Returns the panel that should be used as parent when constructing new
    // TerminalPanels for this tile.
    wxPanel* GetContentArea() const { return contentArea_; }

    // -------------------------------------------------------------------------
    // Tab management (called by UIManager)
    // -------------------------------------------------------------------------

    // Register a session as a new tab.  panel must already be parented to
    // GetContentArea().  Returns the tab index.
    int  AddTab(term::session::SessionId id, TerminalPanel* panel, const wxString& label);

    // Unregister a session tab.  Destroys the panel.
    // Returns true if the tile is now empty (caller should remove it from the grid).
    bool RemoveTab(term::session::SessionId id);

    // Make the tab for the given session the visible one (no callback fired —
    // programmatic activation only).
    void ActivateTabById(term::session::SessionId id);

    // Update the label shown on a specific tab (called when document title changes).
    void SetTabLabel(term::session::SessionId id, const wxString& label);

    int                      GetTabCount()         const { return static_cast<int>(tabs_.size()); }
    term::session::SessionId GetActiveSessionId()  const;
    TerminalPanel*           GetActivePanel()       const;
    int                      GetActiveTabIndex()    const { return activeTabIdx_; }

    // Returns session IDs in tab-insertion order for session-restore snapshots.
    std::vector<term::session::SessionId> GetTabOrder() const;

    // -------------------------------------------------------------------------
    // Tile state (mirrors existing interface used by UIManager / TerminalGrid)
    // -------------------------------------------------------------------------

    // Highlights the title bar to indicate keyboard focus ownership.
    void SetFocused(bool focused);

    // Called by UIManager whenever broadcast mode or group membership changes.
    // modeActive  — broadcast mode is globally on (suppresses the blue "focused" tint
    //               on tiles that are not in the broadcast group).
    // tileHasBroadcast — at least one tab in this tile is in the broadcast group.
    void SetBroadcastState(bool modeActive, bool tileHasBroadcast);

    // Colours an individual tab to show whether its session is in broadcast.
    // Must be called for every session in this tile on each broadcast change.
    void SetTabBroadcast(term::session::SessionId id, bool inBroadcast);

    // Marks a tab as having unread output (content arrived while the tab was hidden).
    // Cleared automatically when the tab is activated.
    void SetTabUnread(term::session::SessionId id, bool hasUnread);

    // Called by UIManager to reflect wrap mode changes originating outside this tile.
    void SetWrapMode(bool wrap);

    // -------------------------------------------------------------------------
    // Callbacks (wired once per tile by UIManager when the tile is created)
    // -------------------------------------------------------------------------

    // Returns the session ID at the given tab slot (0-based), or 0 if out of range.
    // Used by UIManager to collect the ordered session list before a tile move.
    term::session::SessionId GetSessionIdByTabIndex(int index) const;

    // Returns the insert-before tab index for a drop at the given screen point.
    int GetDropIndexAt(wxPoint screenPt) const;

    // Reorder tabs_ in-place: move the tab for `id` to position `insertBefore`.
    // Updates activeTabIdx_ for all move cases; refreshes TabStrip.
    void MoveTabToIndex(term::session::SessionId id, int insertBefore);

    // Fired when the user initiates a whole-tile drag (title bar beyond threshold).
    // Receives a pointer to this tile so UIManager can enumerate all its sessions.
    using DragStartCallback = std::function<void(TerminalTile*, wxPoint)>;
    void SetDragStartCallback(DragStartCallback cb) { dragStartCb_ = std::move(cb); }

    // Fired when the user drags a single tab (TabStrip drag beyond threshold).
    using TabDragStartCallback = std::function<void(term::session::SessionId, wxPoint)>;
    void SetTabDragStartCallback(TabDragStartCallback cb) { tabDragStartCb_ = std::move(cb); }

    // Wired by UIManager via WireTileCallbacks; delegates to App::DropSession.
    // dstTile is this tile for Tabs intent, nullptr for Tile intent (creates new tile).
    using DropSessionCallback =
        std::function<bool(std::span<const term::session::SessionId>, TerminalTile*)>;
    void SetDropSessionCallback(DropSessionCallback cb) { dropSessionCb_ = std::move(cb); }

    // ISessionDropTarget
    bool DropSession(std::span<const term::session::SessionId> ids,
                     ui::DragIntent intent,
                     wxPoint screenPt) override;

    static constexpr int kTitleBarHeight = 28;

private:
    struct TabEntry {
        term::session::SessionId sessionId      = 0;
        TerminalPanel*           panel          = nullptr;  // wx-child-owned by contentArea_
        wxString                 label;
        bool                     inBroadcast    = false;
        bool                     hasUnreadOutput = false;
    };

    // Show the panel at index; hide the previous one. Does not emit ActivateSession — programmatic activation only.
    void ActivateTab(int index);

    void UpdateTitleBarColor();
    void OnSize(wxSizeEvent& evt);
    void OnTitleDown(wxMouseEvent& evt);
    void OnTitleMotion(wxMouseEvent& evt);
    void OnTitleUp(wxMouseEvent& evt);
    void OnTitleRightClick(wxMouseEvent& evt);
    void EmitTerminalAction(TerminalAction action);
    void EmitTileAction(TileAction action, term::session::SessionId id);
    void OnShowTileMenu();

    wxPanel*          titleBar_    = nullptr;  // wx-child-owned
    TabStrip*         tabStrip_    = nullptr;  // wx-child-owned by titleBar_
    WrapControl*      wrapCtrl_    = nullptr;  // wx-child-owned by titleBar_
    wxPanel*          contentArea_ = nullptr;  // wx-child-owned; panels live here

    std::vector<TabEntry>      tabs_;
    int                        activeTabIdx_ = -1;

    DragStartCallback          dragStartCb_;
    TabDragStartCallback       tabDragStartCb_;
    DropSessionCallback        dropSessionCb_;

    wxPoint                    dragAnchor_  { -1, -1 };
    bool                       dragPending_ = false;
    bool                       isFocused_          = false;
    bool                       inBroadcast_        = false;
    bool                       broadcastModeActive_ = false;

    wxColour colActive_    {  45,  57, 160 };
    wxColour colInactive_  { 131, 136, 141 };
    wxColour colBroadcast_ { 255, 140,   0 };
};
