#pragma once
#include <functional>
#include <string>
#include <vector>
#include <wx/panel.h>
#include <wx/bmpbuttn.h>
#include "config/Config.h"
#include "session/ISessionObserver.h"
#include "ui/TerminalActions.h"
#include "ui/TileActions.h"

class TerminalPanel;
class TabStrip;

// TerminalTile is a tab container: one title bar shared across N sessions,
// with only the active session's TerminalPanel visible at a time.
// UIManager creates TerminalPanel instances externally and registers them
// via AddTab(); the tile takes wx-ownership (panels are parented to contentArea_).
class TerminalTile : public wxPanel {
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

    int                      GetTabCount()      const { return static_cast<int>(tabs_.size()); }
    term::session::SessionId GetActiveSessionId() const;
    TerminalPanel*           GetActivePanel()    const;

    // -------------------------------------------------------------------------
    // Tile state (mirrors existing interface used by UIManager / TerminalGrid)
    // -------------------------------------------------------------------------

    // Highlights the title bar to indicate keyboard focus ownership.
    void SetFocused(bool focused);

    // Called by UIManager whenever broadcast mode or group membership changes.
    void SetBroadcastActive(bool active);

    // Called by UIManager to reflect wrap mode changes originating outside this tile.
    void SetWrapMode(bool wrap);

    // -------------------------------------------------------------------------
    // Callbacks (wired once per tile by UIManager when the tile is created)
    // -------------------------------------------------------------------------

    // Fired when the user activates a session (tab click, panel focus).
    using ActivateCallback = std::function<void(term::session::SessionId)>;
    void SetActivateCallback(ActivateCallback cb) { activateCb_ = std::move(cb); }

    // Fired when the user initiates a whole-tile drag (title bar beyond threshold).
    using DragStartCallback = std::function<void(term::session::SessionId, wxPoint)>;
    void SetDragStartCallback(DragStartCallback cb) { dragStartCb_ = std::move(cb); }

    // Fired when the user drags a single tab (TabStrip drag beyond threshold).
    using TabDragStartCallback = std::function<void(term::session::SessionId, wxPoint)>;
    void SetTabDragStartCallback(TabDragStartCallback cb) { tabDragStartCb_ = std::move(cb); }

    // Fired by right-click / Ctrl+Click to toggle active session in broadcast group.
    using BroadcastToggleCallback = std::function<void(term::session::SessionId)>;
    void SetBroadcastToggleCallback(BroadcastToggleCallback cb) { broadcastToggleCb_ = std::move(cb); }

    static constexpr int kTitleBarHeight = 28;

private:
    struct TabEntry {
        term::session::SessionId sessionId = 0;
        TerminalPanel*           panel     = nullptr;  // wx-child-owned by contentArea_
        wxString                 label;
    };

    // Show the panel at index; hide the previous one. Does NOT fire activateCb_.
    void ActivateTab(int index);

    void UpdateTitleBarColor();
    void OnSize(wxSizeEvent& evt);
    void OnTitleDown(wxMouseEvent& evt);
    void OnTitleMotion(wxMouseEvent& evt);
    void OnTitleUp(wxMouseEvent& evt);
    void OnTitleRightClick(wxMouseEvent& evt);
    void OnWrapClick(wxCommandEvent& evt);
    void EmitTerminalAction(TerminalAction action);
    void EmitTileAction(TileAction action, term::session::SessionId id);

    wxPanel*          titleBar_    = nullptr;  // wx-child-owned
    TabStrip*         tabStrip_    = nullptr;  // wx-child-owned by titleBar_
    wxBitmapButton*   wrapBtn_     = nullptr;  // wx-child-owned by titleBar_
    wxPanel*          contentArea_ = nullptr;  // wx-child-owned; panels live here

    std::vector<TabEntry>      tabs_;
    int                        activeTabIdx_ = -1;

    ActivateCallback           activateCb_;
    BroadcastToggleCallback    broadcastToggleCb_;
    DragStartCallback          dragStartCb_;
    TabDragStartCallback       tabDragStartCb_;

    wxPoint                    dragAnchor_  { -1, -1 };
    bool                       dragPending_ = false;
    bool                       isFocused_   = false;
    bool                       inBroadcast_ = false;
    static constexpr int       kDragThreshold = 5;

    wxColour colActive_    {  45,  57, 160 };
    wxColour colInactive_  { 131, 136, 141 };
    wxColour colBroadcast_ { 255, 140,   0 };
};
