#include "ui/NewConnectionDialog.h"

#include <wx/checkbox.h>
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
    constexpr int ID_RB_NORMAL   = wxID_HIGHEST + 202;
    constexpr int ID_RB_WIDE     = wxID_HIGHEST + 203;
    constexpr int ID_CB_WRAP     = wxID_HIGHEST + 204;
}

NewConnectionDialog::NewConnectionDialog(wxWindow* parent, const std::string& defaultShell)
    : wxDialog(parent, wxID_ANY, "New Connection", wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // Transport label
    auto* label = new wxStaticText(this, wxID_ANY, "Transport:");
    outer->Add(label, 0, wxLEFT | wxTOP, 12);

    // Transport radio buttons
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

    // PTY Width mode
    outer->AddSpacer(8);
    outer->Add(new wxStaticText(this, wxID_ANY, "PTY Width:"), 0, wxLEFT, 12);

    m_rbNormal = new wxRadioButton(this, ID_RB_NORMAL, "Normal (follow viewport)",
                                   wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_rbWide   = new wxRadioButton(this, ID_RB_WIDE, "Wide (fixed width)");
    m_rbNormal->SetValue(true);

    outer->Add(m_rbNormal, 0, wxLEFT | wxTOP, 12);
    outer->Add(m_rbWide,   0, wxLEFT | wxTOP, 4);

    // Word wrap checkbox — only meaningful in Wide mode
    m_cbWordWrap = new wxCheckBox(this, ID_CB_WRAP, "Word Wrap");
    m_cbWordWrap->Enable(false);
    outer->Add(m_cbWordWrap, 0, wxLEFT | wxTOP, 24);

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
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnWidthModeChanged, this, ID_RB_NORMAL);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnWidthModeChanged, this, ID_RB_WIDE);
}

void NewConnectionDialog::OnTransportChanged(wxCommandEvent&)
{
    const bool isPty = m_rbPty->GetValue();
    m_shellCtrl->Enable(isPty);
    m_rbNormal->Enable(isPty);
    m_rbWide->Enable(isPty);
    m_cbWordWrap->Enable(isPty && m_rbWide->GetValue());
}

void NewConnectionDialog::OnWidthModeChanged(wxCommandEvent&)
{
    m_cbWordWrap->Enable(m_rbWide->GetValue());
    if (!m_rbWide->GetValue())
        m_cbWordWrap->SetValue(false);
}

ConnectionParams NewConnectionDialog::GetParams() const
{
    if (m_rbPty->GetValue())
        return PtyParams{
            m_shellCtrl->GetValue().ToStdString(),
            m_rbWide->GetValue(),
            m_cbWordWrap->GetValue()
        };
    return LoopbackParams{};
}

} // namespace ui
