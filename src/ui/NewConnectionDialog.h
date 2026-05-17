#pragma once

#include <wx/dialog.h>
#include <string>
#include <variant>
#include <vector>

class wxRadioButton;
class wxCheckBox;
class wxComboBox;
class wxTextCtrl;
class wxPanel;
class wxSpinCtrl;
class wxFilePickerCtrl;

namespace term::db { struct ConnectionProfile; }

namespace ui
{

struct LoopbackParams {
    bool           wrapMode    = true;
    unsigned short columnWidth = 80;
};

struct PtyParams {
    std::string    shell;
    bool           wrapMode    = false;
    unsigned short columnWidth = 80;
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
    bool           wrapMode          = false;
    unsigned short columnWidth       = 80;
};

struct SerialParams {
    std::string    device;
    unsigned int   baudRate     = 115200;
    unsigned short dataBits     = 8;
    std::string    stopBits     = "1";
    std::string    parity       = "None";
    std::string    flowControl  = "None";
    std::string    dialScript;
    bool           wrapMode     = false;
    unsigned short columnWidth  = 80;
};

using ConnectionParams = std::variant<LoopbackParams, PtyParams, SshParams, SerialParams>;

class NewConnectionDialog : public wxDialog
{
public:
    // columnWidths:  list of selectable column widths from AppConfig
    // prefill:       non-null to pre-populate fields for editing a saved profile
    NewConnectionDialog(wxWindow* parent,
                        const std::string& defaultShell,
                        const std::vector<unsigned short>& columnWidths,
                        const term::db::ConnectionProfile* prefill = nullptr);

    // Valid only after ShowModal() == wxID_OK
    ConnectionParams GetParams() const;
    std::string      GetConnectionName() const;
    bool             GetOpenInNewWindow() const;

private:
    void OnTransportChanged(wxCommandEvent&);
    void OnAuthMethodChanged(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    void UpdateLayout();
    void ApplyPrefill(const term::db::ConnectionProfile& profile);

    // Name / open-in-new-window
    wxTextCtrl*    m_nameCtrl         = nullptr;
    wxCheckBox*    m_cbOpenNewWindow  = nullptr;

    // Transport selection
    wxRadioButton* m_rbLoopback   = nullptr;
    wxRadioButton* m_rbPty        = nullptr;
    wxRadioButton* m_rbSsh        = nullptr;
    wxRadioButton* m_rbSerial     = nullptr;

    // PTY fields
    wxTextCtrl*    m_shellCtrl    = nullptr;

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

    // Serial panel (shown/hidden as a unit)
    wxPanel*       m_serialPanel       = nullptr;
    wxTextCtrl*    m_deviceCtrl        = nullptr;
    wxComboBox*    m_baudCtrl          = nullptr;
    wxComboBox*    m_dataBitsCtrl      = nullptr;
    wxComboBox*    m_stopBitsCtrl      = nullptr;
    wxComboBox*    m_parityCtrl        = nullptr;
    wxComboBox*    m_flowCtrlCombo     = nullptr;
    wxTextCtrl*    m_dialScriptCtrl    = nullptr;

    // Shared terminal options (all transports)
    wxComboBox*    m_colWidthCtrl   = nullptr;
    wxCheckBox*    m_cbwrapMode     = nullptr;

    std::vector<unsigned short> m_columnWidths;
};

} // namespace ui
