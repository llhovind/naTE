#include "ui/NewConnectionDialog.h"

#include <wx/radiobut.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>
#include <wx/button.h>

namespace ui
{

namespace
{
    constexpr int ID_RB_LOOPBACK = wxID_HIGHEST + 200;
    constexpr int ID_RB_PTY      = wxID_HIGHEST + 201;
}

NewConnectionDialog::NewConnectionDialog(wxWindow* parent, const std::string& defaultShell)
    : wxDialog(parent, wxID_ANY, "New Connection", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // Transport label
    auto* label = new wxStaticText(this, wxID_ANY, "Transport:");
    outer->Add(label, 0, wxLEFT | wxTOP, 12);

    // Radio buttons
    m_rbLoopback = new wxRadioButton(this, ID_RB_LOOPBACK, "Loopback (local echo)",
                                     wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_rbPty      = new wxRadioButton(this, ID_RB_PTY, "Local Shell (PTY)");
    m_rbPty->SetValue(true);

    outer->Add(m_rbLoopback, 0, wxLEFT | wxTOP, 12);
    outer->Add(m_rbPty,      0, wxLEFT | wxTOP, 4);

    // Shell path row
    auto* shellRow = new wxBoxSizer(wxHORIZONTAL);
    shellRow->Add(new wxStaticText(this, wxID_ANY, "Shell:"), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_shellCtrl = new wxTextCtrl(this, wxID_ANY, defaultShell,
                                 wxDefaultPosition, wxSize(280, -1));
    shellRow->Add(m_shellCtrl, 1, wxEXPAND);
    outer->Add(shellRow, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 12);

    // Separator + standard OK/Cancel buttons
    outer->AddSpacer(8);
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
               wxALIGN_RIGHT | wxALL, 10);

    // Rename OK → "Connect"
    if (auto* btn = dynamic_cast<wxButton*>(FindWindow(wxID_OK)))
        btn->SetLabel("Connect");

    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnTransportChanged, this, ID_RB_LOOPBACK);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnTransportChanged, this, ID_RB_PTY);
}

void NewConnectionDialog::OnTransportChanged(wxCommandEvent&)
{
    m_shellCtrl->Enable(m_rbPty->GetValue());
}

ConnectionParams NewConnectionDialog::GetParams() const
{
    if (m_rbPty->GetValue())
        return PtyParams{m_shellCtrl->GetValue().ToStdString()};
    return LoopbackParams{};
}

} // namespace ui
