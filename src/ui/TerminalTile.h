#pragma once
#include <functional>
#include <string>
#include <wx/panel.h>
#include "config/Config.h"
#include "session/ISessionObserver.h"

class TerminalPanel;
class wxStaticText;

class TerminalTile : public wxPanel {
public:
    TerminalTile(wxWindow* parent, const AppConfig& cfg,
                 unsigned short cols, const std::string& label);

    TerminalPanel* GetTerminalPanel() const { return terminal_; }

    // Highlights the title bar to indicate keyboard focus ownership.
    void SetFocused(bool focused);

    // Updates the text shown in the title bar.
    void SetTileLabel(const wxString& label);

    // UIManager wires this to RequestActivate so title bar clicks reliably
    // activate the session without depending on wxEVT_SET_FOCUS firing.
    using ActivateCallback = std::function<void()>;
    void SetActivateCallback(ActivateCallback cb) { activateCb_ = std::move(cb); }

    // UIManager wires these for drag-to-move session UX.
    using DragStartCallback = std::function<void(term::session::SessionId, wxPoint)>;
    void SetDragStartCallback(DragStartCallback cb) { dragStartCb_ = std::move(cb); }
    void SetTileSessionId(term::session::SessionId id) { tileSessionId_ = id; }

    // UIManager wires this to toggle the session in/out of the broadcast group.
    // Fired by right-click "Add/Remove from Broadcast" and Ctrl+Click.
    using BroadcastToggleCallback = std::function<void()>;
    void SetBroadcastToggleCallback(BroadcastToggleCallback cb) { broadcastToggleCb_ = std::move(cb); }

    // Called by UIManager whenever broadcast mode or group membership changes.
    // active = (mode == Broadcast && this session is in the selected set).
    void SetBroadcastActive(bool active);

    static constexpr int kTitleBarHeight = 24;

private:
    void UpdateTitleBarColor();
    void OnSize(wxSizeEvent& evt);
    void OnTitleDown(wxMouseEvent& evt);
    void OnTitleMotion(wxMouseEvent& evt);
    void OnTitleUp(wxMouseEvent& evt);
    void OnTitleRightClick(wxMouseEvent& evt);

    wxPanel*        titleBar_    = nullptr;  // wx-child-owned
    wxStaticText*   titleText_   = nullptr;  // wx-child-owned by titleBar_
    TerminalPanel*  terminal_    = nullptr;  // wx-child-owned
    ActivateCallback         activateCb_;
    BroadcastToggleCallback  broadcastToggleCb_;

    DragStartCallback        dragStartCb_;
    term::session::SessionId tileSessionId_ = 0;
    wxPoint                  dragAnchor_    {-1, -1};
    bool                     dragPending_   = false;
    bool                     isFocused_     = false;
    bool                     inBroadcast_   = false;
    static constexpr int     kDragThreshold = 5;

    wxColour colActive_    {  60, 100, 160 };
    wxColour colInactive_  {  70,  70,  70 };
    wxColour colBroadcast_ { 175, 130,  40 };
};
