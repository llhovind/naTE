#pragma once

#include <functional>
#include <string>
#include <wx/button.h>
#include <wx/panel.h>
#include <wx/stattext.h>
#include "config/Config.h"

// Full-width status strip that appears at the top of a TerminalPanel when the
// session's transport has been unexpectedly interrupted. Offers three actions:
//   Reconnect  — rebuild the transport with the same connection profile.
//   Save...    — export the current scrollback to a text file.
//   Close      — tear down the session.
//
// Owned by TerminalPanel as a child wx widget; shown/hidden via ShowBar/HideBar.
class ReconnectBar : public wxPanel
{
public:
    using Callback = std::function<void()>;

    explicit ReconnectBar(wxWindow* parent);

    // Apply theme-derived colors.  Called by TerminalPanel::ApplyConfig.
    void ApplyConfig(const AppConfig& cfg);

    // Show the bar with the given disconnect message.
    void ShowBar(const wxString& message);

    // Hide the bar and clear the displayed message.
    void HideBar();

    void SetReconnectCallback(Callback cb) { reconnectCb_ = std::move(cb); }
    void SetSaveCallback(Callback cb)      { saveCb_      = std::move(cb); }
    void SetCloseCallback(Callback cb)     { closeCb_     = std::move(cb); }

private:
    void OnPaint(wxPaintEvent&);
    void OnReconnect(wxCommandEvent&);
    void OnSave(wxCommandEvent&);
    void OnClose(wxCommandEvent&);

    wxStaticText* label_       = nullptr;
    wxButton*     reconnectBtn_= nullptr;
    wxButton*     saveBtn_     = nullptr;
    wxButton*     closeBtn_    = nullptr;

    Callback reconnectCb_;
    Callback saveCb_;
    Callback closeCb_;
};
