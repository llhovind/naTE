#include "ui/AddPortForwardDialog.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/app.h>

namespace ui {

static constexpr int kFallbackMs = 10'000;

// ---------------------------------------------------------------------------

AddPortForwardDialog::AddPortForwardDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Add Port Forward",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
    , liveMode_(false)
{
    fallbackTimer_.SetOwner(this);
    Bind(wxEVT_TIMER, &AddPortForwardDialog::OnFallbackTimer, this,
         fallbackTimer_.GetId());
    BuildUI();
}

AddPortForwardDialog::AddPortForwardDialog(
    wxWindow* parent,
    std::function<void(term::transport::PortForwardDesc, ResultCb)> onSubmit)
    : wxDialog(parent, wxID_ANY, "Add Port Forward",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE)
    , onSubmit_(std::move(onSubmit))
    , liveMode_(true)
{
    fallbackTimer_.SetOwner(this);
    Bind(wxEVT_TIMER, &AddPortForwardDialog::OnFallbackTimer, this,
         fallbackTimer_.GetId());
    BuildUI();
}

// ---------------------------------------------------------------------------

void AddPortForwardDialog::BuildUI()
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* grid = new wxFlexGridSizer(2, wxSize(8, 6));
    grid->AddGrowableCol(1, 1);

    // Direction
    grid->Add(new wxStaticText(this, wxID_ANY, "Direction:"),
              0, wxALIGN_CENTER_VERTICAL);
    dirChoice_ = new wxChoice(this, wxID_ANY);
    dirChoice_->Append("Local  (-L  localPort:remoteHost:remotePort)");
    dirChoice_->Append("Remote (-R  remotePort:connectHost:connectPort)");
    dirChoice_->SetSelection(0);
    dirChoice_->Bind(wxEVT_CHOICE, &AddPortForwardDialog::OnDirectionChanged, this);
    grid->Add(dirChoice_, 1, wxEXPAND);

    // Local Port
    grid->Add(new wxStaticText(this, wxID_ANY, "Local Port:"),
              0, wxALIGN_CENTER_VERTICAL);
    localPortCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                    wxSize(90, -1), wxSP_ARROW_KEYS, 1, 65535, 8080);
    grid->Add(localPortCtrl_, 0);

    // Remote Host (label changes with direction)
    remoteHostLbl_ = new wxStaticText(this, wxID_ANY, "Remote Host:");
    grid->Add(remoteHostLbl_, 0, wxALIGN_CENTER_VERTICAL);
    remoteHostCtrl_ = new wxTextCtrl(this, wxID_ANY, "localhost");
    grid->Add(remoteHostCtrl_, 1, wxEXPAND);

    // Remote Port
    grid->Add(new wxStaticText(this, wxID_ANY, "Remote Port:"),
              0, wxALIGN_CENTER_VERTICAL);
    remotePortCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition,
                                     wxSize(90, -1), wxSP_ARROW_KEYS, 1, 65535, 8080);
    grid->Add(remotePortCtrl_, 0);

    // Bind Address
    grid->Add(new wxStaticText(this, wxID_ANY, "Bind Address:"),
              0, wxALIGN_CENTER_VERTICAL);
    bindAddrCtrl_ = new wxTextCtrl(this, wxID_ANY, "127.0.0.1");
    grid->Add(bindAddrCtrl_, 1, wxEXPAND);

    // Label (optional)
    grid->Add(new wxStaticText(this, wxID_ANY, "Label:"),
              0, wxALIGN_CENTER_VERTICAL);
    labelCtrl_ = new wxTextCtrl(this, wxID_ANY, "");
    grid->Add(labelCtrl_, 1, wxEXPAND);

    outer->Add(grid, 0, wxEXPAND | wxALL, 12);

    // Status line (hidden until needed)
    statusLabel_ = new wxStaticText(this, wxID_ANY, "");
    statusLabel_->Hide();
    outer->Add(statusLabel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // Buttons
    auto* btns = new wxStdDialogButtonSizer();
    addBtn_    = new wxButton(this, wxID_OK,     liveMode_ ? "Add" : "Add");
    cancelBtn_ = new wxButton(this, wxID_CANCEL, "Cancel");
    btns->AddButton(addBtn_);
    btns->AddButton(cancelBtn_);
    btns->Realize();
    addBtn_->SetDefault();
    addBtn_->SetFocus();
    addBtn_->Bind(wxEVT_BUTTON, &AddPortForwardDialog::OnAdd, this);

    outer->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(440, -1));
    Fit();
}

// ---------------------------------------------------------------------------

void AddPortForwardDialog::OnDirectionChanged(wxCommandEvent&)
{
    const bool isRemote = (dirChoice_->GetSelection() == 1);
    remoteHostLbl_->SetLabel(isRemote ? "Connect Host:" : "Remote Host:");
    Layout();
}

bool AddPortForwardDialog::Validate() const
{
    if (localPortCtrl_->GetValue() == 0) return false;
    if (remoteHostCtrl_->GetValue().Trim().empty()) return false;
    if (remotePortCtrl_->GetValue() == 0) return false;
    return true;
}

void AddPortForwardDialog::OnAdd(wxCommandEvent&)
{
    if (!Validate()) {
        SetStatus("Please fill in all required fields.", true);
        return;
    }

    using Dir = term::transport::PortForwardDirection;
    desc_.direction  = (dirChoice_->GetSelection() == 1) ? Dir::Remote : Dir::Local;
    desc_.localPort  = static_cast<uint16_t>(localPortCtrl_->GetValue());
    desc_.remoteHost = remoteHostCtrl_->GetValue().utf8_string();
    desc_.remotePort = static_cast<uint16_t>(remotePortCtrl_->GetValue());
    desc_.bindAddr   = bindAddrCtrl_->GetValue().Trim().utf8_string();
    desc_.label      = labelCtrl_->GetValue().utf8_string();

    if (desc_.bindAddr.empty())
        desc_.bindAddr = "127.0.0.1";

    if (!liveMode_) {
        EndModal(wxID_OK);
        return;
    }

    SetBusy(true);
    SetStatus("Verifying...", false);
    fallbackTimer_.StartOnce(kFallbackMs);

    // The ResultCb is invoked on the UI thread (via wxTheApp->CallAfter inside
    // PortForwardPanel::UpdateStatus), so it fires inside ShowModal's event loop.
    onSubmit_(desc_, [this](bool ok, std::string error) {
        fallbackTimer_.Stop();
        if (ok) {
            EndModal(wxID_OK);
        } else {
            SetBusy(false);
            SetStatus(wxString::FromUTF8(error.empty() ? "Setup failed." : error), true);
        }
    });
}

void AddPortForwardDialog::OnFallbackTimer(wxTimerEvent&)
{
    SetBusy(false);
    SetStatus(wxString::FromUTF8("Timed out \xe2\x80\x94 the session may have disconnected."), true);
}

void AddPortForwardDialog::DeliverResult(bool ok, const std::string& error)
{
    fallbackTimer_.Stop();
    if (ok) {
        EndModal(wxID_OK);
    } else {
        SetBusy(false);
        SetStatus(wxString::FromUTF8(error.empty() ? "Setup failed." : error), true);
    }
}

void AddPortForwardDialog::SetStatus(const wxString& msg, bool isError)
{
    if (msg.empty()) {
        statusLabel_->Hide();
    } else {
        statusLabel_->SetLabel(msg);
        // Use red for errors, neutral for info messages like "Verifying..."
        const wxColour col = isError
            ? wxColour(220, 60, 60)
            : GetForegroundColour();
        statusLabel_->SetForegroundColour(col);
        statusLabel_->Show();
    }
    Layout();
    Fit();
}

void AddPortForwardDialog::SetBusy(bool busy)
{
    addBtn_->Enable(!busy);
    cancelBtn_->Enable(!busy);
}

} // namespace ui
