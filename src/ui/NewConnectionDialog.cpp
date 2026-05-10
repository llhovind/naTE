#include "ui/NewConnectionDialog.h"
#include "db/ConnectionProfile.h"
#include "session/Connection.h"

#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/filepicker.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/sizer.h>
#include <wx/button.h>

namespace ui
{

namespace
{
    constexpr int ID_RB_LOOPBACK    = wxID_HIGHEST + 200;
    constexpr int ID_RB_PTY         = wxID_HIGHEST + 201;
    constexpr int ID_RB_SSH         = wxID_HIGHEST + 202;
    constexpr int ID_RB_AUTH_AGENT  = wxID_HIGHEST + 205;
    constexpr int ID_RB_AUTH_PASS   = wxID_HIGHEST + 206;
    constexpr int ID_RB_AUTH_KEY    = wxID_HIGHEST + 207;
}

NewConnectionDialog::NewConnectionDialog(wxWindow* parent,
                                         const std::string& defaultShell,
                                         const std::vector<unsigned short>& columnWidths,
                                         const term::db::ConnectionProfile* prefill)
    : wxDialog(parent, wxID_ANY,
               prefill ? "Edit Connection" : "New Connection",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_columnWidths(columnWidths.empty() ? std::vector<unsigned short>{80, 132} : columnWidths)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- Name ---------------------------------------------------------------
    auto* nameRow = new wxBoxSizer(wxHORIZONTAL);
    nameRow->Add(new wxStaticText(this, wxID_ANY, "Name:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_nameCtrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(280, -1));
    nameRow->Add(m_nameCtrl, 1, wxEXPAND);
    outer->Add(nameRow, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 12);

    // ---- Transport selection ------------------------------------------------
    outer->Add(new wxStaticText(this, wxID_ANY, "Transport:"), 0, wxLEFT | wxTOP, 12);

    m_rbLoopback = new wxRadioButton(this, ID_RB_LOOPBACK, "Loopback (local echo)",
                                     wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_rbPty      = new wxRadioButton(this, ID_RB_PTY, "Local Shell (PTY)");
    m_rbSsh      = new wxRadioButton(this, ID_RB_SSH, "SSH");
    m_rbPty->SetValue(true);

    outer->Add(m_rbLoopback, 0, wxLEFT | wxTOP, 4);
    outer->Add(m_rbPty,      0, wxLEFT | wxTOP, 4);
    outer->Add(m_rbSsh,      0, wxLEFT | wxTOP, 4);

    // ---- PTY fields ---------------------------------------------------------
    auto* shellRow = new wxBoxSizer(wxHORIZONTAL);
    shellRow->Add(new wxStaticText(this, wxID_ANY, "Shell:"), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_shellCtrl = new wxTextCtrl(this, wxID_ANY, defaultShell,
                                 wxDefaultPosition, wxSize(280, -1));
    shellRow->Add(m_shellCtrl, 1, wxEXPAND);
    outer->Add(shellRow, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 12);

    // ---- SSH panel ----------------------------------------------------------
    m_sshPanel = new wxPanel(this, wxID_ANY);
    auto* sshSizer = new wxBoxSizer(wxVERTICAL);

    // Connection fields grid
    auto* grid = new wxFlexGridSizer(2, wxSize(8, 4));
    grid->AddGrowableCol(1);

    grid->Add(new wxStaticText(m_sshPanel, wxID_ANY, "Host:"),
              0, wxALIGN_CENTER_VERTICAL);
    m_hostCtrl = new wxTextCtrl(m_sshPanel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(260, -1));
    grid->Add(m_hostCtrl, 1, wxEXPAND);

    grid->Add(new wxStaticText(m_sshPanel, wxID_ANY, "Port:"),
              0, wxALIGN_CENTER_VERTICAL);
    m_portCtrl = new wxSpinCtrl(m_sshPanel, wxID_ANY, "22",
                                wxDefaultPosition, wxSize(80, -1),
                                wxSP_ARROW_KEYS, 1, 65535, 22);
    grid->Add(m_portCtrl, 0);

    grid->Add(new wxStaticText(m_sshPanel, wxID_ANY, "Username:"),
              0, wxALIGN_CENTER_VERTICAL);
    m_userCtrl = new wxTextCtrl(m_sshPanel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(260, -1));
    grid->Add(m_userCtrl, 1, wxEXPAND);

    grid->Add(new wxStaticText(m_sshPanel, wxID_ANY, "Timeout (s):"),
              0, wxALIGN_CENTER_VERTICAL);
    m_timeoutCtrl = new wxSpinCtrl(m_sshPanel, wxID_ANY, "10",
                                   wxDefaultPosition, wxSize(80, -1),
                                   wxSP_ARROW_KEYS, 1, 120, 10);
    grid->Add(m_timeoutCtrl, 0);

    sshSizer->Add(grid, 0, wxEXPAND | wxALL, 4);
    sshSizer->AddSpacer(6);

    // Authentication box
    auto* authBox = new wxStaticBoxSizer(wxVERTICAL, m_sshPanel, "Authentication");

    m_rbAuthAgent = new wxRadioButton(m_sshPanel, ID_RB_AUTH_AGENT,
                                      "SSH Agent (SSH_AUTH_SOCK)",
                                      wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_rbAuthPass  = new wxRadioButton(m_sshPanel, ID_RB_AUTH_PASS, "Password");
    m_rbAuthKey   = new wxRadioButton(m_sshPanel, ID_RB_AUTH_KEY,  "Private Key File");
    m_rbAuthAgent->SetValue(true);

    authBox->Add(m_rbAuthAgent, 0, wxALL, 4);

    // Password sub-panel
    m_passPanel = new wxPanel(m_sshPanel, wxID_ANY);
    auto* passSizer = new wxBoxSizer(wxHORIZONTAL);
    passSizer->Add(new wxStaticText(m_passPanel, wxID_ANY, "Password:"),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_passCtrl = new wxTextCtrl(m_passPanel, wxID_ANY, wxEmptyString,
                                wxDefaultPosition, wxSize(220, -1), wxTE_PASSWORD);
    passSizer->Add(m_passCtrl, 1, wxEXPAND);
    m_passPanel->SetSizer(passSizer);
    m_passPanel->Show(false);

    authBox->Add(m_rbAuthPass,  0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
    authBox->Add(m_passPanel,   0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // Private key sub-panel
    m_keyPanel = new wxPanel(m_sshPanel, wxID_ANY);
    auto* keySizer = new wxBoxSizer(wxVERTICAL);

    auto* keyRow = new wxBoxSizer(wxHORIZONTAL);
    keyRow->Add(new wxStaticText(m_keyPanel, wxID_ANY, "Key file:"),
                0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_keyPicker = new wxFilePickerCtrl(m_keyPanel, wxID_ANY, wxEmptyString,
                                       "Select private key",
                                       "All files (*)|*",
                                       wxDefaultPosition, wxSize(240, -1),
                                       wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
    keyRow->Add(m_keyPicker, 1, wxEXPAND);
    keySizer->Add(keyRow, 0, wxEXPAND | wxBOTTOM, 4);

    auto* ppRow = new wxBoxSizer(wxHORIZONTAL);
    ppRow->Add(new wxStaticText(m_keyPanel, wxID_ANY, "Passphrase:"),
               0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    m_passphraseCtrl = new wxTextCtrl(m_keyPanel, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxSize(220, -1), wxTE_PASSWORD);
    ppRow->Add(m_passphraseCtrl, 1, wxEXPAND);
    keySizer->Add(ppRow, 0, wxEXPAND);
    m_keyPanel->SetSizer(keySizer);
    m_keyPanel->Show(false);

    authBox->Add(m_rbAuthKey,  0, wxLEFT | wxRIGHT | wxBOTTOM, 4);
    authBox->Add(m_keyPanel,   0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    sshSizer->Add(authBox, 0, wxEXPAND | wxALL, 4);
    sshSizer->AddSpacer(4);

    // SSH options row
    auto* optRow = new wxBoxSizer(wxHORIZONTAL);
    optRow->Add(new wxStaticText(m_sshPanel, wxID_ANY, "Keep-alive (s):"),
                0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_keepaliveCtrl = new wxSpinCtrl(m_sshPanel, wxID_ANY, "30",
                                     wxDefaultPosition, wxSize(60, -1),
                                     wxSP_ARROW_KEYS, 0, 300, 30);
    optRow->Add(m_keepaliveCtrl, 0, wxRIGHT, 12);

    optRow->Add(new wxStaticText(m_sshPanel, wxID_ANY, "Command:"),
                0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_remoteCmdCtrl = new wxTextCtrl(m_sshPanel, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(140, -1));
    optRow->Add(m_remoteCmdCtrl, 1, wxRIGHT, 12);

    m_cbCompress = new wxCheckBox(m_sshPanel, wxID_ANY, "Compress");
    optRow->Add(m_cbCompress, 0, wxALIGN_CENTER_VERTICAL);

    sshSizer->Add(optRow, 0, wxEXPAND | wxALL, 4);

    m_sshPanel->SetSizer(sshSizer);
    m_sshPanel->Show(false);
    outer->Add(m_sshPanel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

    // ---- Shared terminal options --------------------------------------------
    outer->AddSpacer(8);
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->AddSpacer(6);

    auto* termOptRow = new wxBoxSizer(wxHORIZONTAL);
    termOptRow->Add(new wxStaticText(this, wxID_ANY, "Column width:"),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

    wxArrayString widthChoices;
    for (unsigned short w : m_columnWidths)
        widthChoices.Add(wxString::Format("%d", static_cast<int>(w)));
    m_colWidthCtrl = new wxComboBox(this, wxID_ANY, widthChoices.empty() ? "80" : widthChoices[0],
                                    wxDefaultPosition, wxSize(80, -1),
                                    widthChoices, wxCB_READONLY);
    if (!widthChoices.IsEmpty())
        m_colWidthCtrl->SetSelection(0);
    termOptRow->Add(m_colWidthCtrl, 0, wxRIGHT, 20);

    m_cbwrapMode = new wxCheckBox(this, wxID_ANY, "Wrap mode");
    m_cbwrapMode->SetValue(false);
    termOptRow->Add(m_cbwrapMode, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);

    // Hidden when editing a saved profile (prefill path).
    m_cbOpenNewWindow = new wxCheckBox(this, wxID_ANY, "Open in New Window");
    m_cbOpenNewWindow->SetValue(false);
    m_cbOpenNewWindow->Show(prefill == nullptr);
    termOptRow->Add(m_cbOpenNewWindow, 0, wxALIGN_CENTER_VERTICAL);

    outer->Add(termOptRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // ---- Footer -------------------------------------------------------------
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->Add(CreateButtonSizer(wxOK | wxCANCEL), 0,
               wxALIGN_RIGHT | wxALL, 10);

    if (auto* btn = dynamic_cast<wxButton*>(FindWindow(wxID_OK)))
        btn->SetLabel(prefill ? "Save" : "Connect");

    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    // Events
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnTransportChanged, this, ID_RB_LOOPBACK);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnTransportChanged, this, ID_RB_PTY);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnTransportChanged, this, ID_RB_SSH);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnAuthMethodChanged, this, ID_RB_AUTH_AGENT);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnAuthMethodChanged, this, ID_RB_AUTH_PASS);
    Bind(wxEVT_RADIOBUTTON, &NewConnectionDialog::OnAuthMethodChanged, this, ID_RB_AUTH_KEY);
    Bind(wxEVT_BUTTON, &NewConnectionDialog::OnOK, this, wxID_OK);

    if (prefill)
        ApplyPrefill(*prefill);
}

void NewConnectionDialog::ApplyPrefill(const term::db::ConnectionProfile& profile)
{
    m_nameCtrl->SetValue(profile.name);

    std::visit([this](const auto& desc) {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, term::session::PtyDesc>) {
            m_rbPty->SetValue(true);
            m_shellCtrl->SetValue(desc.shell);
            m_sshPanel->Show(false);
            m_shellCtrl->Enable(true);
        } else if constexpr (std::is_same_v<T, term::session::SshDesc>) {
            m_rbSsh->SetValue(true);
            m_hostCtrl->SetValue(desc.host);
            m_portCtrl->SetValue(desc.port);
            m_userCtrl->SetValue(desc.username);
            m_timeoutCtrl->SetValue(desc.connectTimeoutSec);
            m_keepaliveCtrl->SetValue(desc.keepaliveSeconds);
            m_remoteCmdCtrl->SetValue(desc.remoteCommand);
            m_cbCompress->SetValue(desc.compress);
            switch (desc.authMethod) {
                case term::session::SshAuthMethod::Password:
                    m_rbAuthPass->SetValue(true);
                    m_passPanel->Show(true);
                    break;
                case term::session::SshAuthMethod::PrivateKey:
                    m_rbAuthKey->SetValue(true);
                    m_keyPicker->SetPath(desc.privateKeyPath);
                    m_keyPanel->Show(true);
                    break;
                default:
                    m_rbAuthAgent->SetValue(true);
                    break;
            }
            m_sshPanel->Show(true);
            m_shellCtrl->Enable(false);
        } else {
            // Loopback
            m_rbLoopback->SetValue(true);
            m_sshPanel->Show(false);
            m_shellCtrl->Enable(false);
        }
    }, profile.transport);

    // Apply shared settings
    m_cbwrapMode->SetValue(profile.wrapMode);
    for (int i = 0; i < static_cast<int>(m_columnWidths.size()); ++i) {
        if (m_columnWidths[i] == profile.columnWidth) {
            m_colWidthCtrl->SetSelection(i);
            break;
        }
    }

    // Sync wrap mode state for Loopback
    if (m_rbLoopback->GetValue()) {
        m_cbwrapMode->SetValue(true);
        m_cbwrapMode->Enable(false);
    }

    UpdateLayout();
}

void NewConnectionDialog::OnTransportChanged(wxCommandEvent&)
{
    const bool isPty      = m_rbPty->GetValue();
    const bool isSsh      = m_rbSsh->GetValue();
    const bool isLoopback = m_rbLoopback->GetValue();

    m_shellCtrl->Enable(isPty);
    m_sshPanel->Show(isSsh);

    // Loopback: wrap mode is always on and not user-adjustable
    m_cbwrapMode->SetValue(isLoopback ? true : m_cbwrapMode->GetValue());
    m_cbwrapMode->Enable(!isLoopback);

    UpdateLayout();
}

void NewConnectionDialog::OnAuthMethodChanged(wxCommandEvent&)
{
    m_passPanel->Show(m_rbAuthPass->GetValue());
    m_keyPanel->Show(m_rbAuthKey->GetValue());
    m_sshPanel->Layout();
    UpdateLayout();
}

void NewConnectionDialog::OnOK(wxCommandEvent& evt)
{
    if (m_rbSsh->GetValue()) {
        if (m_hostCtrl->GetValue().IsEmpty()) {
            wxMessageBox("Please enter a hostname.", "SSH Connection",
                         wxOK | wxICON_WARNING, this);
            m_hostCtrl->SetFocus();
            return;
        }
        if (m_userCtrl->GetValue().IsEmpty()) {
            wxMessageBox("Please enter a username.", "SSH Connection",
                         wxOK | wxICON_WARNING, this);
            m_userCtrl->SetFocus();
            return;
        }
    }
    evt.Skip(); // allow default OK processing
}

void NewConnectionDialog::UpdateLayout()
{
    Layout();
    Fit();
    SetMinSize(GetSize());
}

ConnectionParams NewConnectionDialog::GetParams() const
{
    const int selIdx = m_colWidthCtrl->GetSelection();
    const unsigned short colWidth =
        (selIdx >= 0 && selIdx < static_cast<int>(m_columnWidths.size()))
        ? m_columnWidths[selIdx]
        : 80;
    const bool wrapMode = m_cbwrapMode->GetValue();

    if (m_rbPty->GetValue())
        return PtyParams{
            m_shellCtrl->GetValue().ToStdString(),
            wrapMode,
            colWidth};

    if (m_rbSsh->GetValue()) {
        SshParams p;
        p.host              = m_hostCtrl->GetValue().ToStdString();
        p.port              = static_cast<unsigned short>(m_portCtrl->GetValue());
        p.username          = m_userCtrl->GetValue().ToStdString();
        p.connectTimeoutSec = m_timeoutCtrl->GetValue();
        p.keepaliveSeconds  = m_keepaliveCtrl->GetValue();
        p.remoteCommand     = m_remoteCmdCtrl->GetValue().ToStdString();
        p.compress          = m_cbCompress->GetValue();
        p.wrapMode          = wrapMode;
        p.columnWidth       = colWidth;

        if (m_rbAuthPass->GetValue()) {
            p.authMethod = SshAuthChoice::Password;
            p.password   = m_passCtrl->GetValue().ToStdString();
        } else if (m_rbAuthKey->GetValue()) {
            p.authMethod      = SshAuthChoice::PrivateKey;
            p.privateKeyPath  = m_keyPicker->GetPath().ToStdString();
            p.passphrase      = m_passphraseCtrl->GetValue().ToStdString();
        } else {
            p.authMethod = SshAuthChoice::Agent;
        }
        return p;
    }

    return LoopbackParams{true, colWidth};
}

std::string NewConnectionDialog::GetConnectionName() const
{
    return m_nameCtrl->GetValue().Trim().ToStdString();
}

bool NewConnectionDialog::GetOpenInNewWindow() const
{
    return m_cbOpenNewWindow->IsShown() && m_cbOpenNewWindow->GetValue();
}

} // namespace ui
