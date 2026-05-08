#pragma once

#include <wx/dialog.h>
#include <string>
#include <variant>

class wxRadioButton;
class wxCheckBox;
class wxTextCtrl;
class wxPanel;
class wxSpinCtrl;
class wxFilePickerCtrl;

namespace ui
{

struct LoopbackParams {};

struct PtyParams {
    std::string shell;
    bool        wordWrap = false;
};

enum class SshAuthChoice { Agent, Password, PrivateKey };

struct SshParams {
    std::string    host;
    unsigned short port              = 22;
    std::string    username;
    SshAuthChoice  authMethod        = SshAuthChoice::Agent;
    std::string    password;
    std::string    privateKeyPath;
    std::string    passphrase;
    int            keepaliveSeconds  = 30;
    int            connectTimeoutSec = 10;
    std::string    remoteCommand;
    bool           compress          = false;
};

using ConnectionParams = std::variant<LoopbackParams, PtyParams, SshParams>;

class NewConnectionDialog : public wxDialog
{
public:
    NewConnectionDialog(wxWindow* parent, const std::string& defaultShell);

    // Valid only after ShowModal() == wxID_OK
    ConnectionParams GetParams() const;

private:
    void OnTransportChanged(wxCommandEvent&);
    void OnAuthMethodChanged(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    void UpdateLayout();

    // Transport selection
    wxRadioButton* m_rbLoopback   = nullptr;
    wxRadioButton* m_rbPty        = nullptr;
    wxRadioButton* m_rbSsh        = nullptr;

    // PTY fields
    wxTextCtrl*    m_shellCtrl    = nullptr;
    wxCheckBox*    m_cbWordWrap   = nullptr;

    // SSH panel (shown/hidden as a unit)
    wxPanel*       m_sshPanel     = nullptr;

    // SSH connection fields
    wxTextCtrl*    m_hostCtrl     = nullptr;
    wxSpinCtrl*    m_portCtrl     = nullptr;
    wxTextCtrl*    m_userCtrl     = nullptr;
    wxSpinCtrl*    m_timeoutCtrl  = nullptr;

    // SSH auth
    wxRadioButton* m_rbAuthAgent  = nullptr;
    wxRadioButton* m_rbAuthPass   = nullptr;
    wxRadioButton* m_rbAuthKey    = nullptr;

    // Password auth sub-panel
    wxPanel*       m_passPanel    = nullptr;
    wxTextCtrl*    m_passCtrl     = nullptr;

    // Private key auth sub-panel
    wxPanel*       m_keyPanel     = nullptr;
    wxFilePickerCtrl* m_keyPicker = nullptr;
    wxTextCtrl*    m_passphraseCtrl = nullptr;

    // SSH options
    wxSpinCtrl*    m_keepaliveCtrl  = nullptr;
    wxTextCtrl*    m_remoteCmdCtrl  = nullptr;
    wxCheckBox*    m_cbCompress     = nullptr;
};

} // namespace ui
