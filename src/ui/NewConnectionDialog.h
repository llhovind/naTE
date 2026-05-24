#pragma once

#include "config/Config.h"
#include "db/ConnectionProfile.h"
#include "session/EnvVar.h"

#include <wx/dialog.h>
#include <optional>
#include <string>
#include <variant>
#include <vector>

class wxBookCtrlEvent;
class wxButton;
class wxCheckBox;
class wxChoice;
class wxCollapsiblePane;
class wxCollapsiblePaneEvent;
class wxComboBox;
class wxListBox;
class wxNotebook;
class wxPanel;
class wxRadioBox;
class wxRadioButton;
class wxSimplebook;
class wxSpinCtrl;
class wxTextCtrl;
class wxFilePickerCtrl;
class wxTimer;
class wxTimerEvent;

namespace term::db { struct ConnectionProfile; }

namespace ui
{

// ---------------------------------------------------------------------------
// Launch context — caller tells the dialog how it is being used.
// ---------------------------------------------------------------------------
enum class LaunchContext {
    ActiveTile,   // connection goes into the current tile; placement UI hidden
    UserChoice,   // user picks placement (Current Tile / New Tile / New Window)
    ProfileOnly,  // just create/edit a profile; no placement options or Save-as checkbox
};

// Result of GetLaunchPlacement() — only meaningful for ActiveTile/UserChoice contexts.
enum class LaunchPlacement {
    ActiveTile,
    NewTile,
    NewWindow,
};

// ---------------------------------------------------------------------------
// ConnectionParams — transport-specific data returned by GetParams()
// ---------------------------------------------------------------------------
struct PtyParams {
    std::string    shell;
    bool           wrapMode       = false;
    unsigned short columnWidth    = 80;
    unsigned short rows           = 24;
    // Session Init
    std::string                        workingDir;
    std::vector<term::session::EnvVar> envVars;
    std::string                        envFilePath;
    bool                               loginShell     = false;
    // Profile title override
    std::string    profileTitle;
    bool           useProfileTitle = false;
};

enum class SshAuthChoice { Agent, Password, PrivateKey, KeyboardInteractive };

struct SshParams {
    // Nested jump-host descriptor (nullopt = direct connection).
    struct ProxyJumpParams {
        std::string    host;
        unsigned short port            = 22;
        std::string    user;           // empty = same as SshParams::username
        SshAuthChoice  authMethod      = SshAuthChoice::Agent;
        std::string    password;
        std::string    privateKeyPath;
        std::string    passphrase;
        std::string    agentIdentityHint;
    };

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
    bool           x11Forwarding     = false;
    bool           agentForwarding   = false;
    std::string    agentIdentityHint;
    std::optional<ProxyJumpParams> proxyJump;   // nullopt = direct connection
    bool           wrapMode          = false;
    unsigned short columnWidth       = 80;
    unsigned short rows              = 24;
    // Session Init
    std::string                        workingDir;
    std::vector<term::session::EnvVar> envVars;
    std::string                        envFilePath;
    // Profile title override
    std::string    profileTitle;
    bool           useProfileTitle = false;
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
    unsigned short rows         = 24;
    // Session Init (env vars only for Serial)
    std::vector<term::session::EnvVar> envVars;
    std::string                        envFilePath;
    // Profile title override
    std::string    profileTitle;
    bool           useProfileTitle = false;
};

using ConnectionParams = std::variant<PtyParams, SshParams, SerialParams>;

// ---------------------------------------------------------------------------
// NewConnectionDialog
// ---------------------------------------------------------------------------
class NewConnectionDialog : public wxDialog
{
public:
    // existingProfiles: shown in Profile combo for pre-fill; ignored for ProfileOnly context.
    // prefill: non-null to pre-populate all fields when editing a saved profile.
    NewConnectionDialog(wxWindow* parent,
                        const std::string& defaultShell,
                        const std::string& defaultWorkingDir,
                        bool defaultWrapMode,
                        bool defaultLoginShell,
                        const std::vector<GeometryPreset>& geometryPresets,
                        const std::vector<term::db::ConnectionProfile>& existingProfiles,
                        LaunchContext context,
                        const term::db::ConnectionProfile* prefill = nullptr);
    ~NewConnectionDialog();

    // Valid only after ShowModal() == wxID_OK
    ConnectionParams GetParams()           const;
    std::string      GetProfileName()      const;  // trimmed value from profile field
    bool             GetSaveAsProfile()    const;  // true only for ActiveTile/UserChoice + checkbox checked
    std::string      GetSelectedProfileId() const; // ID of combo-selected profile; empty = new
    LaunchPlacement  GetLaunchPlacement()  const;

private:
    // Notebook tab indices (file-local constants in .cpp)
    static constexpr int kTabPty    = 0;
    static constexpr int kTabSsh    = 1;
    static constexpr int kTabSerial = 2;

    void UpdateConnectButton();
    void OnConnectClicked(wxCommandEvent&);
    void OnCollapsiblePaneChanged(wxCollapsiblePaneEvent&);
    void OnTabChanged(wxBookCtrlEvent&);
    void OnSshFieldChanged(wxCommandEvent&);
    void OnHostDetectTimer(wxTimerEvent&);
    void DetectProxyJumpFromConfig();
    void OnAuthMethodChanged(wxCommandEvent&);
    void OnGeometryChanged(wxCommandEvent&);
    void OnProfileSelected(wxCommandEvent&);
    void OnProfileTextChanged(wxCommandEvent&);
    void OnBrowseWorkingDir(wxCommandEvent&);
    void OnAddEnvVar(wxCommandEvent&);
    void OnEditEnvVar(wxCommandEvent&);
    void OnRemoveEnvVar(wxCommandEvent&);
    void OnEnvVarSelected(wxCommandEvent&);
    void OnUseProfileTitleToggled(wxCommandEvent&);

    void ApplyPrefill(const term::db::ConnectionProfile& profile);

    // Context
    LaunchContext m_context;

    // Profile field (one of the two is non-null depending on context)
    wxComboBox*  m_profileCombo    = nullptr;  // ActiveTile / UserChoice
    wxTextCtrl*  m_profileNameCtrl = nullptr;  // ProfileOnly
    wxCheckBox*  m_cbSaveProfile   = nullptr;  // shown for ActiveTile / UserChoice only
    wxButton*    m_connectBtn      = nullptr;  // ActiveTile / UserChoice primary action
    std::string  m_selectedProfileId;
    std::vector<term::db::ConnectionProfile> m_existingProfiles;

    // Transport notebook
    wxNotebook* m_notebook = nullptr;

    // PTY tab fields
    wxTextCtrl*  m_shellCtrl       = nullptr;
    wxTextCtrl*  m_ptyWorkDirCtrl  = nullptr;
    wxButton*    m_ptyWorkDirBtn   = nullptr;
    wxCheckBox*  m_cbPtyLoginShell = nullptr;

    // SSH tab connection fields
    wxTextCtrl*      m_hostCtrl        = nullptr;
    wxTextCtrl*      m_portCtrl        = nullptr;  // plain numeric text input
    wxTextCtrl*      m_userCtrl        = nullptr;
    wxTimer*         m_hostDetectTimer = nullptr;  // debounce for ssh-config ProxyJump lookup

    // SSH Jump Host collapsible section
    wxCollapsiblePane* m_jumpPane          = nullptr;
    wxTextCtrl*        m_jumpHostCtrl      = nullptr;
    wxTextCtrl*        m_jumpPortCtrl      = nullptr;
    wxTextCtrl*        m_jumpUserCtrl      = nullptr;
    wxChoice*          m_jumpAuthChoice    = nullptr;
    wxSimplebook*      m_jumpAuthBook      = nullptr;  // pages: Agent/Password/PrivKey/KbdInt
    wxFilePickerCtrl*  m_jumpHintPicker    = nullptr;  // Agent page
    wxTextCtrl*        m_jumpPassCtrl      = nullptr;  // Password page
    wxFilePickerCtrl*  m_jumpKeyPicker     = nullptr;  // PrivKey page
    wxTextCtrl*        m_jumpPassphraseCtrl = nullptr; // PrivKey page
    void OnJumpAuthChoiceChanged(wxCommandEvent&);
    wxSpinCtrl*      m_timeoutCtrl     = nullptr;
    wxRadioButton*   m_rbAuthAgent     = nullptr;
    wxRadioButton*   m_rbAuthPass      = nullptr;
    wxRadioButton*   m_rbAuthKey       = nullptr;
    wxRadioButton*   m_rbAuthKbd       = nullptr;
    wxPanel*          m_agentPanel      = nullptr;
    wxFilePickerCtrl* m_agentHintPicker = nullptr;
    wxPanel*          m_passPanel       = nullptr;
    wxTextCtrl*       m_passCtrl        = nullptr;
    wxPanel*          m_keyPanel        = nullptr;
    wxFilePickerCtrl* m_keyPicker       = nullptr;
    wxTextCtrl*       m_passphraseCtrl  = nullptr;
    wxSpinCtrl*      m_keepaliveCtrl   = nullptr;
    wxTextCtrl*      m_remoteCmdCtrl   = nullptr;
    wxCheckBox*      m_cbCompress      = nullptr;
    wxCheckBox*      m_cbX11Fwd        = nullptr;
    wxCheckBox*      m_cbAgentFwd      = nullptr;
    wxTextCtrl*      m_sshWorkDirCtrl  = nullptr;
    wxButton*        m_sshWorkDirBtn   = nullptr;

    // Serial tab connection fields
    wxTextCtrl*  m_deviceCtrl     = nullptr;
    wxComboBox*  m_baudCtrl       = nullptr;
    wxComboBox*  m_dataBitsCtrl   = nullptr;
    wxComboBox*  m_stopBitsCtrl   = nullptr;
    wxComboBox*  m_parityCtrl     = nullptr;
    wxComboBox*  m_flowCtrlCombo  = nullptr;
    wxTextCtrl*  m_dialScriptCtrl = nullptr;

    // Shared terminal options
    wxComboBox*  m_geometryCombo     = nullptr;
    wxPanel*     m_customGeomPanel   = nullptr;
    wxSpinCtrl*  m_colsCtrl          = nullptr;
    wxSpinCtrl*  m_rowsCtrl          = nullptr;
    wxCheckBox*  m_cbWrapMode        = nullptr;
    wxTextCtrl*  m_profileTitleCtrl  = nullptr;
    wxCheckBox*  m_cbUseProfileTitle = nullptr;

    // Placement (UserChoice only)
    wxRadioBox* m_placementCtrl = nullptr;

    // Shared environment variables (single panel, collapsible)
    wxCollapsiblePane* m_envPane    = nullptr;
    wxFilePickerCtrl*  m_envFileCtrl = nullptr;
    wxListBox*         m_envVarList  = nullptr;
    wxButton*          m_btnAddEnv   = nullptr;
    wxButton*          m_btnEditEnv  = nullptr;
    wxButton*          m_btnRemoveEnv = nullptr;

    std::vector<GeometryPreset> m_geometryPresets;
};

} // namespace ui
