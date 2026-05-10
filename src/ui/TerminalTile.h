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

    static constexpr int kTitleBarHeight = 24;

private:
    void OnSize(wxSizeEvent& evt);
    void OnTitleDown(wxMouseEvent& evt);
    void OnTitleMotion(wxMouseEvent& evt);
    void OnTitleUp(wxMouseEvent& evt);

    wxPanel*        titleBar_    = nullptr;  // wx-child-owned
    wxStaticText*   titleText_   = nullptr;  // wx-child-owned by titleBar_
    TerminalPanel*  terminal_    = nullptr;  // wx-child-owned
    ActivateCallback activateCb_;

    DragStartCallback        dragStartCb_;
    term::session::SessionId tileSessionId_ = 0;
    wxPoint                  dragAnchor_    {-1, -1};
    bool                     dragPending_   = false;
    static constexpr int     kDragThreshold = 5;

    wxColour colActive_   { 60, 100, 160 };
    wxColour colInactive_ { 70,  70,  70 };
};
