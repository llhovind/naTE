#pragma once

#include "config/Config.h"
#include "db/ConnectionProfile.h"

#include <wx/dialog.h>
#include <string>
#include <variant>
#include <vector>

class wxCheckBox;
class wxComboBox;
class wxNotebook;
class wxPanel;
class wxRadioBox;
class wxRadioButton;
class wxSpinCtrl;
class wxTextCtrl;
class wxFilePickerCtrl;

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
    bool           wrapMode    = false;
    unsigned short columnWidth = 80;
    unsigned short rows        = 24;
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
    unsigned short rows              = 24;
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
                        const std::vector<GeometryPreset>& geometryPresets,
                        const std::vector<term::db::ConnectionProfile>& existingProfiles,
                        LaunchContext context,
                        const term::db::ConnectionProfile* prefill = nullptr);

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

    void OnAuthMethodChanged(wxCommandEvent&);
    void OnGeometryChanged(wxCommandEvent&);
    void OnProfileSelected(wxCommandEvent&);
    void OnProfileTextChanged(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    void ApplyPrefill(const term::db::ConnectionProfile& profile);

    // Context
    LaunchContext m_context;

    // Profile field (one of the two is non-null depending on context)
    wxComboBox*  m_profileCombo    = nullptr;  // ActiveTile / UserChoice
    wxTextCtrl*  m_profileNameCtrl = nullptr;  // ProfileOnly
    wxCheckBox*  m_cbSaveProfile   = nullptr;  // shown for ActiveTile / UserChoice only
    std::string  m_selectedProfileId;
    std::vector<term::db::ConnectionProfile> m_existingProfiles;

    // Transport notebook
    wxNotebook* m_notebook = nullptr;

    // PTY tab fields
    wxTextCtrl* m_shellCtrl = nullptr;

    // SSH tab fields
    wxTextCtrl*      m_hostCtrl        = nullptr;
    wxSpinCtrl*      m_portCtrl        = nullptr;
    wxTextCtrl*      m_userCtrl        = nullptr;
    wxSpinCtrl*      m_timeoutCtrl     = nullptr;
    wxRadioButton*   m_rbAuthAgent     = nullptr;
    wxRadioButton*   m_rbAuthPass      = nullptr;
    wxRadioButton*   m_rbAuthKey       = nullptr;
    wxPanel*         m_passPanel       = nullptr;
    wxTextCtrl*      m_passCtrl        = nullptr;
    wxPanel*         m_keyPanel        = nullptr;
    wxFilePickerCtrl* m_keyPicker      = nullptr;
    wxTextCtrl*      m_passphraseCtrl  = nullptr;
    wxSpinCtrl*      m_keepaliveCtrl   = nullptr;
    wxTextCtrl*      m_remoteCmdCtrl   = nullptr;
    wxCheckBox*      m_cbCompress      = nullptr;

    // Serial tab fields
    wxTextCtrl*  m_deviceCtrl     = nullptr;
    wxComboBox*  m_baudCtrl       = nullptr;
    wxComboBox*  m_dataBitsCtrl   = nullptr;
    wxComboBox*  m_stopBitsCtrl   = nullptr;
    wxComboBox*  m_parityCtrl     = nullptr;
    wxComboBox*  m_flowCtrlCombo  = nullptr;
    wxTextCtrl*  m_dialScriptCtrl = nullptr;

    // Shared terminal options
    wxComboBox*  m_geometryCombo   = nullptr;
    wxPanel*     m_customGeomPanel = nullptr;
    wxSpinCtrl*  m_colsCtrl        = nullptr;
    wxSpinCtrl*  m_rowsCtrl        = nullptr;
    wxCheckBox*  m_cbWrapMode      = nullptr;

    // Placement (UserChoice only)
    wxRadioBox* m_placementCtrl = nullptr;

    std::vector<GeometryPreset> m_geometryPresets;
};

} // namespace ui
