#pragma once
#include "transport/PortForward.h"
#include <functional>
#include <string>
#include <wx/dialog.h>
#include <wx/timer.h>

class wxButton;
class wxChoice;
class wxStaticText;
class wxTextCtrl;

namespace ui {

// Modal dialog for adding a port forward.
//
// Two construction modes:
//   Profile mode  — no onSubmit; [Add] closes immediately after local validation.
//   Live mode     — onSubmit triggers transport setup; dialog waits for the
//                   transport to confirm success/failure before closing.
//
// In live mode the ResultCb is invoked on the UI thread (inside the modal
// event loop via wxTheApp->CallAfter) by PortForwardPanel::UpdateStatus().
class AddPortForwardDialog : public wxDialog
{
public:
    using ResultCb = std::function<void(bool ok, std::string error)>;

    // Profile mode: closes immediately on [Add] after local validation.
    explicit AddPortForwardDialog(wxWindow* parent);

    // Live mode: calls onSubmit with the filled desc + a result callback.
    AddPortForwardDialog(
        wxWindow* parent,
        std::function<void(term::transport::PortForwardDesc, ResultCb)> onSubmit);

    term::transport::PortForwardDesc GetDesc() const { return desc_; }

private:
    void BuildUI();
    void OnDirectionChanged(wxCommandEvent&);
    void OnAdd(wxCommandEvent&);
    void OnFallbackTimer(wxTimerEvent&);
    void SetStatus(const wxString& msg, bool isError);
    void SetBusy(bool busy);
    bool Validate() const;

    wxChoice*     dirChoice_      = nullptr;
    wxTextCtrl*   localPortCtrl_  = nullptr;
    wxStaticText* remoteHostLbl_  = nullptr;
    wxTextCtrl*   remoteHostCtrl_ = nullptr;
    wxTextCtrl*   remotePortCtrl_ = nullptr;
    wxTextCtrl*   bindAddrCtrl_ = nullptr;
    wxTextCtrl*   labelCtrl_    = nullptr;
    wxStaticText* statusLabel_  = nullptr;
    wxButton*     addBtn_       = nullptr;
    wxButton*     cancelBtn_    = nullptr;
    wxTimer       fallbackTimer_;

    std::function<void(term::transport::PortForwardDesc, ResultCb)> onSubmit_;
    term::transport::PortForwardDesc desc_;
    bool liveMode_ = false;
};

} // namespace ui
