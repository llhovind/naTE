#pragma once
#include <functional>
#include <string>
#include <wx/panel.h>
#include "config/Config.h"

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

    static constexpr int kTitleBarHeight = 24;

private:
    void OnSize(wxSizeEvent& evt);

    wxPanel*        titleBar_    = nullptr;  // wx-child-owned
    wxStaticText*   titleText_   = nullptr;  // wx-child-owned by titleBar_
    TerminalPanel*  terminal_    = nullptr;  // wx-child-owned
    ActivateCallback activateCb_;

    wxColour colActive_   { 60, 100, 160 };
    wxColour colInactive_ { 70,  70,  70 };
};
