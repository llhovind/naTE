#include "ui/NewConnectionDialog.h"
#include "db/ConnectionProfile.h"
#include "session/Connection.h"
#include "transport/SshConfig.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/combobox.h>
#include <wx/dirdlg.h>
#include <wx/filepicker.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/radiobox.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/textdlg.h>

#include <algorithm>

namespace ui
{

namespace
{
    constexpr int ID_SSH_AUTH_CHOICE      = wxID_HIGHEST + 205;
    constexpr int ID_JUMP_AUTH_CHOICE     = wxID_HIGHEST + 231;
    constexpr int ID_GEOMETRY_COMBO       = wxID_HIGHEST + 208;
    constexpr int ID_PROFILE_COMBO        = wxID_HIGHEST + 209;
    constexpr int ID_BTN_BROWSE_WORKDIR_PTY = wxID_HIGHEST + 210;
    constexpr int ID_BTN_BROWSE_WORKDIR_SSH = wxID_HIGHEST + 211;
    constexpr int ID_LIST_ENV             = wxID_HIGHEST + 212;
    constexpr int ID_BTN_ADD_ENV          = wxID_HIGHEST + 215;
    constexpr int ID_BTN_EDIT_ENV         = wxID_HIGHEST + 216;
    constexpr int ID_BTN_REMOVE_ENV       = wxID_HIGHEST + 217;
    constexpr int ID_CB_USE_PROFILE_TITLE = wxID_HIGHEST + 224;
    constexpr int ID_BTN_CONNECT          = wxID_HIGHEST + 225;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
NewConnectionDialog::NewConnectionDialog(
        wxWindow* parent,
        const std::string& defaultShell,
        const std::string& defaultWorkingDir,
        bool defaultWrapMode,
        bool defaultLoginShell,
        const std::vector<GeometryPreset>& geometryPresets,
        const std::vector<term::db::ConnectionProfile>& existingProfiles,
        LaunchContext context,
        const term::db::ConnectionProfile* prefill)
    : wxDialog(parent, wxID_ANY,
               prefill ? "Edit Connection" : "New Connection",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_context(context)
    , m_existingProfiles(existingProfiles)
    , m_geometryPresets(geometryPresets.empty()
          ? std::vector<GeometryPreset>{{80, 24}, {132, 24}}
          : geometryPresets)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- Profile + Connect row -----------------------------------------------
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, "Profile:"),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

        if (context == LaunchContext::ProfileOnly) {
            m_profileNameCtrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                               wxDefaultPosition, wxSize(280, -1));
            if (prefill)
                m_profileNameCtrl->SetValue(prefill->name);
            row->Add(m_profileNameCtrl, 1, wxEXPAND);
        } else {
            wxArrayString names;
            for (const auto& p : m_existingProfiles)
                names.Add(p.name);
            m_profileCombo = new wxComboBox(this, ID_PROFILE_COMBO, wxEmptyString,
                                            wxDefaultPosition, wxSize(280, -1),
                                            names, wxCB_DROPDOWN);
            m_profileCombo->AutoComplete(names);
            row->Add(m_profileCombo, 1, wxEXPAND | wxRIGHT, 8);

            m_connectBtn = new wxButton(this, ID_BTN_CONNECT, "Connect");
            m_connectBtn->Enable(false);
            row->Add(m_connectBtn, 0, wxALIGN_CENTER_VERTICAL);
        }

        outer->Add(row, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 12);
    }

    // ---- Save as Profile + Open In (combined row, below profile) -------------
    // "Save as Profile" left-aligned; "Open In" right-aligned (UserChoice only).
    // Row is omitted entirely for ProfileOnly (neither control is shown).
    {
        const bool showSave      = (context != LaunchContext::ProfileOnly);
        const bool showPlacement = (context == LaunchContext::UserChoice);

        if (showSave || showPlacement) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            if (showSave) {
                m_cbSaveProfile = new wxCheckBox(this, wxID_ANY, "Save as Profile");
                row->Add(m_cbSaveProfile, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 50);
            }

            if (showPlacement) {
                row->AddStretchSpacer();
                wxArrayString opts;
                opts.Add("Current Tile"); opts.Add("New Tile"); opts.Add("New Window");
                m_placementCtrl = new wxRadioBox(this, wxID_ANY, "Open in",
                                                 wxDefaultPosition, wxDefaultSize,
                                                 opts, 3, wxRA_SPECIFY_COLS );
                m_placementCtrl->SetSelection(1);  // default: New Tile
                row->Add(m_placementCtrl, 0, wxALIGN_CENTER_VERTICAL);
            }

            outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        }
    }

    outer->AddSpacer(8);
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->AddSpacer(6);

    // ---- Shared terminal options (Geometry · Wrap · Override Title) ----------
    {
        auto* termRow = new wxBoxSizer(wxHORIZONTAL);
        termRow->Add(new wxStaticText(this, wxID_ANY, "Geometry:"),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

        wxArrayString geoChoices;
        for (const auto& g : m_geometryPresets)
            geoChoices.Add(wxString::Format("%dx%d", g.cols, g.rows));
        geoChoices.Add("Custom");

        m_geometryCombo = new wxComboBox(this, ID_GEOMETRY_COMBO,
                                         geoChoices.IsEmpty() ? "80x24" : geoChoices[0],
                                         wxDefaultPosition, wxSize(100, -1),
                                         geoChoices, wxCB_READONLY);
        m_geometryCombo->SetSelection(0);
        termRow->Add(m_geometryCombo, 0, wxRIGHT, 8);

        // Custom geometry sub-panel (hidden until "Custom" selected)
        m_customGeomPanel = new wxPanel(this, wxID_ANY);
        {
            auto* s = new wxBoxSizer(wxHORIZONTAL);
            s->Add(new wxStaticText(m_customGeomPanel, wxID_ANY, "Cols:"),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_colsCtrl = new wxSpinCtrl(m_customGeomPanel, wxID_ANY, "80",
                                        wxDefaultPosition, wxDefaultSize,
                                        wxSP_ARROW_KEYS, 10, 1000, 80);
            s->Add(m_colsCtrl, 0, wxRIGHT, 8);
            s->Add(new wxStaticText(m_customGeomPanel, wxID_ANY, "Rows:"),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_rowsCtrl = new wxSpinCtrl(m_customGeomPanel, wxID_ANY, "24",
                                        wxDefaultPosition, wxDefaultSize,
                                        wxSP_ARROW_KEYS, 1, 200, 24);
            s->Add(m_rowsCtrl, 0);
            m_customGeomPanel->SetSizer(s);
        }
        m_customGeomPanel->Show(false);
        termRow->Add(m_customGeomPanel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

        m_cbWrapMode = new wxCheckBox(this, wxID_ANY, "Wrap mode");
        m_cbWrapMode->SetValue(defaultWrapMode);
        termRow->Add(m_cbWrapMode, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);

        // Override Title — right side of the same row
        termRow->AddStretchSpacer();
        m_cbUseProfileTitle = new wxCheckBox(this, ID_CB_USE_PROFILE_TITLE, "Override title:");
        termRow->Add(m_cbUseProfileTitle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_profileTitleCtrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(180, -1));
        m_profileTitleCtrl->SetHint("(empty = transport-provided)");
        m_profileTitleCtrl->Enable(false);
        termRow->Add(m_profileTitleCtrl, 0);

        outer->Add(termRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }

    outer->AddSpacer(2);
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->AddSpacer(6);

    // ---- Transport notebook -------------------------------------------------
    m_notebook = new wxNotebook(this, wxID_ANY);

    // --- PTY page ---
    {
        auto* page = new wxPanel(m_notebook, wxID_ANY);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* shellRow = new wxBoxSizer(wxHORIZONTAL);
        shellRow->Add(new wxStaticText(page, wxID_ANY, "Shell:"),
                      0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_shellCtrl = new wxTextCtrl(page, wxID_ANY, defaultShell,
                                     wxDefaultPosition, wxSize(280, -1));
        shellRow->Add(m_shellCtrl, 1, wxEXPAND);
        sizer->Add(shellRow, 0, wxEXPAND | wxALL, 12);

        // Session Init
        {
            auto* box = new wxStaticBoxSizer(wxVERTICAL, page, "Session Init");

            // FlexGrid aligns the input field left-edges for both rows
            auto* initGrid = new wxFlexGridSizer(2, 2, 4, 6);
            initGrid->AddGrowableCol(1);

            initGrid->Add(new wxStaticText(page, wxID_ANY, "Working directory:"),
                          0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
            {
                auto* inner = new wxBoxSizer(wxHORIZONTAL);
                m_ptyWorkDirCtrl = new wxTextCtrl(page, wxID_ANY,
                                                  wxString::FromUTF8(defaultWorkingDir),
                                                  wxDefaultPosition, wxSize(160, -1));
                inner->Add(m_ptyWorkDirCtrl, 1, wxEXPAND | wxRIGHT, 4);
                m_ptyWorkDirBtn = new wxButton(page, ID_BTN_BROWSE_WORKDIR_PTY, "Browse...");
                inner->Add(m_ptyWorkDirBtn, 0);
                initGrid->Add(inner, 1, wxEXPAND);
            }

            initGrid->Add(new wxStaticText(page, wxID_ANY, "Command:"),
                          0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
            m_ptyCmdCtrl = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxSize(280, -1));
            m_ptyCmdCtrl->SetHint("(optional) command to run in shell");
            initGrid->Add(m_ptyCmdCtrl, 1, wxEXPAND);

            box->Add(initGrid, 0, wxEXPAND | wxALL, 4);

            m_cbPtyLoginShell = new wxCheckBox(page, wxID_ANY,
                                               "Login shell (sources .profile / .bash_profile)");
            m_cbPtyLoginShell->SetValue(defaultLoginShell);
            box->Add(m_cbPtyLoginShell, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        }

        page->SetSizer(sizer);
        m_notebook->AddPage(page, "Local Shell");
    }

    // --- SSH page ---
    {
        auto* page = new wxPanel(m_notebook, wxID_ANY);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        // Host + Port and Username — FlexGrid keeps input left-edges aligned
        {
            auto* grid = new wxFlexGridSizer(2, 2, 4, 6);
            grid->AddGrowableCol(1);

            grid->Add(new wxStaticText(page, wxID_ANY, "Host:"),
                      0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
            {
                auto* inner = new wxBoxSizer(wxHORIZONTAL);
                m_hostCtrl = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(220, -1));
                inner->Add(m_hostCtrl, 1, wxEXPAND | wxRIGHT, 12);
                inner->Add(new wxStaticText(page, wxID_ANY, "Port:"),
                           0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_portCtrl = new wxTextCtrl(page, wxID_ANY, "22",
                                            wxDefaultPosition, wxSize(55, -1));
                inner->Add(m_portCtrl, 0);
                grid->Add(inner, 1, wxEXPAND);
            }

            grid->Add(new wxStaticText(page, wxID_ANY, "Username:"),
                      0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
            m_userCtrl = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                        wxDefaultPosition, wxSize(260, -1));
            m_userCtrl->SetHint("defaults to current user");
            grid->Add(m_userCtrl, 1, wxEXPAND);

            sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        }

        // Via Jump Host (ProxyJump) — collapsible, collapsed by default
        m_jumpPane = new wxCollapsiblePane(page, wxID_ANY, "Via Jump Host");
        m_jumpPane->Collapse(true);
        {
            auto* pw = m_jumpPane->GetPane();
            auto* ps = new wxBoxSizer(wxVERTICAL);

            // Host + Port and Username — FlexGrid keeps input left-edges aligned
            {
                auto* grid = new wxFlexGridSizer(2, 2, 4, 6);
                grid->AddGrowableCol(1);

                grid->Add(new wxStaticText(pw, wxID_ANY, "Host:"),
                          0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
                {
                    auto* inner = new wxBoxSizer(wxHORIZONTAL);
                    m_jumpHostCtrl = new wxTextCtrl(pw, wxID_ANY, wxEmptyString,
                                                    wxDefaultPosition, wxSize(200, -1));
                    inner->Add(m_jumpHostCtrl, 1, wxEXPAND | wxRIGHT, 12);
                    inner->Add(new wxStaticText(pw, wxID_ANY, "Port:"),
                               0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                    m_jumpPortCtrl = new wxTextCtrl(pw, wxID_ANY, "22",
                                                    wxDefaultPosition, wxSize(55, -1));
                    inner->Add(m_jumpPortCtrl, 0);
                    grid->Add(inner, 1, wxEXPAND);
                }

                grid->Add(new wxStaticText(pw, wxID_ANY, "Username:"),
                          0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
                m_jumpUserCtrl = new wxTextCtrl(pw, wxID_ANY, wxEmptyString,
                                                wxDefaultPosition, wxSize(180, -1));
                m_jumpUserCtrl->SetHint("defaults to target user");
                grid->Add(m_jumpUserCtrl, 1, wxEXPAND);

                ps->Add(grid, 0, wxEXPAND | wxBOTTOM, 4);
            }

            // Auth: dropdown + compact simplebook on the same row
            {
                auto* r = new wxBoxSizer(wxHORIZONTAL);
                r->Add(new wxStaticText(pw, wxID_ANY, "Auth:"),
                       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_jumpAuthChoice = new wxChoice(pw, ID_JUMP_AUTH_CHOICE);
                m_jumpAuthChoice->Append("SSH Agent");
                m_jumpAuthChoice->Append("Password");
                m_jumpAuthChoice->Append("Private Key File");
                m_jumpAuthChoice->Append("Keyboard Interactive");
                m_jumpAuthChoice->SetSelection(0);
                r->Add(m_jumpAuthChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

                m_jumpAuthBook = new wxSimplebook(pw, wxID_ANY);

                // Page 0: Agent — identity hint (single row; optional)
                {
                    auto* pg = new wxPanel(m_jumpAuthBook, wxID_ANY);
                    auto* s  = new wxBoxSizer(wxHORIZONTAL);
                    s->Add(new wxStaticText(pg, wxID_ANY, "Identity hint:"),
                           0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                    m_jumpHintPicker = new wxFilePickerCtrl(
                        pg, wxID_ANY, wxEmptyString,
                        "Select private key to prefer for agent auth", "All files (*)|*",
                        wxDefaultPosition, wxSize(200, -1),
                        wxFLP_OPEN | wxFLP_USE_TEXTCTRL);
                    if (m_jumpHintPicker->GetTextCtrl())
                        m_jumpHintPicker->GetTextCtrl()->SetHint("(optional)");
                    s->Add(m_jumpHintPicker, 1, wxEXPAND);
                    pg->SetSizer(s);
                    m_jumpAuthBook->AddPage(pg, "Agent");
                }

                // Page 1: Password (single row)
                {
                    auto* pg = new wxPanel(m_jumpAuthBook, wxID_ANY);
                    auto* s  = new wxBoxSizer(wxHORIZONTAL);
                    s->Add(new wxStaticText(pg, wxID_ANY, "Password:"),
                           0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                    m_jumpPassCtrl = new wxTextCtrl(pg, wxID_ANY, wxEmptyString,
                                                    wxDefaultPosition, wxSize(180, -1), wxTE_PASSWORD);
                    s->Add(m_jumpPassCtrl, 1, wxALIGN_CENTER_VERTICAL);
                    pg->SetSizer(s);
                    m_jumpAuthBook->AddPage(pg, "Password");
                }

                // Page 2: Private Key — key file + passphrase (two rows)
                {
                    auto* pg = new wxPanel(m_jumpAuthBook, wxID_ANY);
                    auto* s  = new wxBoxSizer(wxVERTICAL);
                    {
                        auto* r = new wxBoxSizer(wxHORIZONTAL);
                        r->Add(new wxStaticText(pg, wxID_ANY, "Key file:"),
                               0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                        m_jumpKeyPicker = new wxFilePickerCtrl(
                            pg, wxID_ANY, wxEmptyString,
                            "Select private key", "All files (*)|*",
                            wxDefaultPosition, wxSize(200, -1),
                            wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
                        r->Add(m_jumpKeyPicker, 1, wxEXPAND);
                        s->Add(r, 0, wxEXPAND | wxBOTTOM, 4);
                    }
                    {
                        auto* r = new wxBoxSizer(wxHORIZONTAL);
                        r->Add(new wxStaticText(pg, wxID_ANY, "Passphrase:"),
                               0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                        m_jumpPassphraseCtrl = new wxTextCtrl(pg, wxID_ANY, wxEmptyString,
                                                              wxDefaultPosition, wxSize(180, -1),
                                                              wxTE_PASSWORD);
                        r->Add(m_jumpPassphraseCtrl, 1, 0);
                        s->Add(r, 0, wxEXPAND);
                    }
                    pg->SetSizer(s);
                    m_jumpAuthBook->AddPage(pg, "PrivKey");
                }

                // Page 3: Keyboard Interactive (no extra fields)
                {
                    auto* pg = new wxPanel(m_jumpAuthBook, wxID_ANY);
                    pg->SetSizer(new wxBoxSizer(wxVERTICAL));
                    m_jumpAuthBook->AddPage(pg, "KbdInt");
                }

                r->Add(m_jumpAuthBook, 1, wxALIGN_CENTER_VERTICAL);
                ps->Add(r, 0, wxEXPAND | wxBOTTOM, 4);
            }

            auto* padded = new wxBoxSizer(wxVERTICAL);
            padded->Add(ps, 1, wxEXPAND | wxALL, 8);
            pw->SetSizer(padded);
        }

        // Authentication box — dropdown + compact simplebook (consistent with Jump Host)
        {
            auto* authBox = new wxStaticBoxSizer(wxVERTICAL, page, "Authentication");

            auto* authRow = new wxBoxSizer(wxHORIZONTAL);
            authRow->Add(new wxStaticText(page, wxID_ANY, "Auth:"),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

            m_authChoice = new wxChoice(page, ID_SSH_AUTH_CHOICE);
            m_authChoice->Append("SSH Agent");
            m_authChoice->Append("Password");
            m_authChoice->Append("Private Key File");
            m_authChoice->Append("Keyboard Interactive");
            m_authChoice->SetSelection(0);
            authRow->Add(m_authChoice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

            m_authBook = new wxSimplebook(page, wxID_ANY);

            // Page 0: Agent — identity hint (single row; optional)
            {
                auto* pg = new wxPanel(m_authBook, wxID_ANY);
                auto* s  = new wxBoxSizer(wxHORIZONTAL);
                s->Add(new wxStaticText(pg, wxID_ANY, "Identity hint:"),
                       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_agentHintPicker = new wxFilePickerCtrl(
                    pg, wxID_ANY, wxEmptyString,
                    "Select private key to prefer for agent auth", "All files (*)|*",
                    wxDefaultPosition, wxSize(200, -1),
                    wxFLP_OPEN | wxFLP_USE_TEXTCTRL);
                if (m_agentHintPicker->GetTextCtrl())
                    m_agentHintPicker->GetTextCtrl()->SetHint("(optional)");
                s->Add(m_agentHintPicker, 1, wxEXPAND);
                pg->SetSizer(s);
                m_authBook->AddPage(pg, "Agent");
            }

            // Page 1: Password (single row)
            {
                auto* pg = new wxPanel(m_authBook, wxID_ANY);
                auto* s  = new wxBoxSizer(wxHORIZONTAL);
                s->Add(new wxStaticText(pg, wxID_ANY, "Password:"),
                       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_passCtrl = new wxTextCtrl(pg, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(200, -1), wxTE_PASSWORD);
                s->Add(m_passCtrl, 1, wxALIGN_CENTER_VERTICAL);
                pg->SetSizer(s);
                m_authBook->AddPage(pg, "Password");
            }

            // Page 2: Private Key — key file + passphrase (two rows)
            {
                auto* pg = new wxPanel(m_authBook, wxID_ANY);
                auto* s  = new wxBoxSizer(wxVERTICAL);
                {
                    auto* r = new wxBoxSizer(wxHORIZONTAL);
                    r->Add(new wxStaticText(pg, wxID_ANY, "Key file:"),
                           0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                    m_keyPicker = new wxFilePickerCtrl(
                        pg, wxID_ANY, wxEmptyString,
                        "Select private key", "All files (*)|*",
                        wxDefaultPosition, wxSize(200, -1),
                        wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
                    r->Add(m_keyPicker, 1, wxEXPAND);
                    s->Add(r, 0, wxEXPAND | wxBOTTOM, 4);
                }
                {
                    auto* r = new wxBoxSizer(wxHORIZONTAL);
                    r->Add(new wxStaticText(pg, wxID_ANY, "Passphrase:"),
                           0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                    m_passphraseCtrl = new wxTextCtrl(pg, wxID_ANY, wxEmptyString,
                                                      wxDefaultPosition, wxSize(200, -1),
                                                      wxTE_PASSWORD);
                    r->Add(m_passphraseCtrl, 1, 0);
                    s->Add(r, 0, wxEXPAND);
                }
                pg->SetSizer(s);
                m_authBook->AddPage(pg, "PrivKey");
            }

            // Page 3: Keyboard Interactive (no extra fields)
            {
                auto* pg = new wxPanel(m_authBook, wxID_ANY);
                pg->SetSizer(new wxBoxSizer(wxVERTICAL));
                m_authBook->AddPage(pg, "KbdInt");
            }

            authRow->Add(m_authBook, 1, wxALIGN_CENTER_VERTICAL);
            authBox->Add(authRow, 0, wxEXPAND | wxALL, 6);

            sizer->Add(authBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        }

        sizer->Add(m_jumpPane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);

        // Options row — X11 · Agent · Compress · Timeout · Keep-alive
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            m_cbX11Fwd = new wxCheckBox(page, wxID_ANY, "Forward X11");
            row->Add(m_cbX11Fwd, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

            m_cbAgentFwd = new wxCheckBox(page, wxID_ANY, "Forward Agent");
            row->Add(m_cbAgentFwd, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

            m_cbCompress = new wxCheckBox(page, wxID_ANY, "Compress");
            row->Add(m_cbCompress, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 20);

            row->Add(new wxStaticText(page, wxID_ANY, "Timeout (s):"),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_timeoutCtrl = new wxSpinCtrl(page, wxID_ANY, "10",
                                           wxDefaultPosition, wxDefaultSize,
                                           wxSP_ARROW_KEYS, 1, 120, 10);
            row->Add(m_timeoutCtrl, 0, wxRIGHT, 12);

            row->Add(new wxStaticText(page, wxID_ANY, "Keep-alive (s):"),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_keepaliveCtrl = new wxSpinCtrl(page, wxID_ANY, "30",
                                             wxDefaultPosition, wxDefaultSize,
                                             wxSP_ARROW_KEYS, 0, 300, 30);
            row->Add(m_keepaliveCtrl, 0);

            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 8);
        }

        // Session Init
        {
            auto* box = new wxStaticBoxSizer(wxVERTICAL, page, "Session Init");

            // FlexGrid aligns the input field left-edges for both rows
            auto* initGrid = new wxFlexGridSizer(2, 2, 4, 6);
            initGrid->AddGrowableCol(1);

            initGrid->Add(new wxStaticText(page, wxID_ANY, "Working directory:"),
                          0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT);
            {
                auto* inner = new wxBoxSizer(wxHORIZONTAL);
                m_sshWorkDirCtrl = new wxTextCtrl(page, wxID_ANY,
                                                  wxString::FromUTF8(defaultWorkingDir),
                                                  wxDefaultPosition, wxSize(160, -1));
                inner->Add(m_sshWorkDirCtrl, 1, wxEXPAND | wxRIGHT, 4);
                m_sshWorkDirBtn = new wxButton(page, ID_BTN_BROWSE_WORKDIR_SSH, "Browse...");
                inner->Add(m_sshWorkDirBtn, 0);
                initGrid->Add(inner, 1, wxEXPAND);
            }

            initGrid->Add(new wxStaticText(page, wxID_ANY, "Command:"),
                          0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT );
            m_remoteCmdCtrl = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                             wxDefaultPosition, wxSize(280, -1));
            m_remoteCmdCtrl->SetHint("(optional) remote command");
            initGrid->Add(m_remoteCmdCtrl, 1, wxEXPAND);

            box->Add(initGrid, 0, wxEXPAND | wxALL, 4);

            m_cbSshLoginShell = new wxCheckBox(page, wxID_ANY,
                                               "Login shell (sources .profile / .bash_profile)");
            m_cbSshLoginShell->SetValue(defaultLoginShell);
            box->Add(m_cbSshLoginShell, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

            sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, 8);
        }

        page->SetSizer(sizer);
        m_notebook->AddPage(page, "SSH");
    }

    // --- Serial page ---
    {
        auto* page = new wxPanel(m_notebook, wxID_ANY);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* grid = new wxFlexGridSizer(2, wxSize(8, 4));
        grid->AddGrowableCol(1);

        grid->Add(new wxStaticText(page, wxID_ANY, "Device:"),
                  0, wxALIGN_CENTER_VERTICAL);
        m_deviceCtrl = new wxTextCtrl(page, wxID_ANY, "/dev/ttyUSB0",
                                      wxDefaultPosition, wxSize(200, -1));
        grid->Add(m_deviceCtrl, 1, wxEXPAND);

        grid->Add(new wxStaticText(page, wxID_ANY, "Baud rate:"),
                  0, wxALIGN_CENTER_VERTICAL);
        {
            wxArrayString bauds;
            for (const char* b : {"9600","19200","38400","57600",
                                   "115200","230400","460800","921600"})
                bauds.Add(b);
            m_baudCtrl = new wxComboBox(page, wxID_ANY, "115200",
                                        wxDefaultPosition, wxSize(100, -1),
                                        bauds, wxCB_READONLY);
            m_baudCtrl->SetStringSelection("115200");
        }
        grid->Add(m_baudCtrl, 0);

        grid->Add(new wxStaticText(page, wxID_ANY, "Data bits:"),
                  0, wxALIGN_CENTER_VERTICAL);
        {
            wxArrayString db;
            for (const char* d : {"5","6","7","8"}) db.Add(d);
            m_dataBitsCtrl = new wxComboBox(page, wxID_ANY, "8",
                                            wxDefaultPosition, wxSize(60, -1),
                                            db, wxCB_READONLY);
            m_dataBitsCtrl->SetStringSelection("8");
        }
        grid->Add(m_dataBitsCtrl, 0);

        grid->Add(new wxStaticText(page, wxID_ANY, "Stop bits:"),
                  0, wxALIGN_CENTER_VERTICAL);
        {
            wxArrayString sb;
            sb.Add("1"); sb.Add("2");
            m_stopBitsCtrl = new wxComboBox(page, wxID_ANY, "1",
                                            wxDefaultPosition, wxSize(60, -1),
                                            sb, wxCB_READONLY);
            m_stopBitsCtrl->SetStringSelection("1");
        }
        grid->Add(m_stopBitsCtrl, 0);

        grid->Add(new wxStaticText(page, wxID_ANY, "Parity:"),
                  0, wxALIGN_CENTER_VERTICAL);
        {
            wxArrayString par;
            par.Add("None"); par.Add("Even"); par.Add("Odd");
            m_parityCtrl = new wxComboBox(page, wxID_ANY, "None",
                                          wxDefaultPosition, wxSize(100, -1),
                                          par, wxCB_READONLY);
            m_parityCtrl->SetStringSelection("None");
        }
        grid->Add(m_parityCtrl, 0);

        grid->Add(new wxStaticText(page, wxID_ANY, "Flow control:"),
                  0, wxALIGN_CENTER_VERTICAL);
        {
            wxArrayString fc;
            fc.Add("None"); fc.Add("Hardware"); fc.Add("Software");
            m_flowCtrlCombo = new wxComboBox(page, wxID_ANY, "None",
                                             wxDefaultPosition, wxSize(120, -1),
                                             fc, wxCB_READONLY);
            m_flowCtrlCombo->SetStringSelection("None");
        }
        grid->Add(m_flowCtrlCombo, 0);

        grid->Add(new wxStaticText(page, wxID_ANY, "Dial script:"),
                  0, wxALIGN_CENTER_VERTICAL);
        m_dialScriptCtrl = new wxTextCtrl(page, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxSize(200, -1));
        m_dialScriptCtrl->SetHint("(optional) path to dial script");
        grid->Add(m_dialScriptCtrl, 1, wxEXPAND);

        sizer->Add(grid, 0, wxEXPAND | wxALL, 12);

        page->SetSizer(sizer);
        m_notebook->AddPage(page, "Serial");
    }

    m_notebook->SetSelection(kTabPty);
    outer->Add(m_notebook, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    // ---- Environment Variables (shared, collapsible) -------------------------
    {
        m_envPane = new wxCollapsiblePane(this, wxID_ANY, "Environment Variables");
        m_envPane->Collapse(true);
        auto* pw = m_envPane->GetPane();
        auto* ps = new wxBoxSizer(wxVERTICAL);

        auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
        fileRow->Add(new wxStaticText(pw, wxID_ANY, "Env file:"),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_envFileCtrl = new wxFilePickerCtrl(
            pw, wxID_ANY, wxEmptyString, "Select .env file",
            "Env files (*.env)|*.env|All files (*)|*",
            wxDefaultPosition, wxSize(200, -1), wxFLP_OPEN | wxFLP_USE_TEXTCTRL);
        fileRow->Add(m_envFileCtrl, 1, wxEXPAND);
        ps->Add(fileRow, 0, wxEXPAND | wxALL, 4);

        m_envVarList = new wxListBox(pw, ID_LIST_ENV,
                                     wxDefaultPosition, wxSize(-1, 70));
        ps->Add(m_envVarList, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        m_btnAddEnv    = new wxButton(pw, ID_BTN_ADD_ENV,    "Add...");
        m_btnEditEnv   = new wxButton(pw, ID_BTN_EDIT_ENV,   "Edit...");
        m_btnRemoveEnv = new wxButton(pw, ID_BTN_REMOVE_ENV, "Remove");
        m_btnEditEnv->Enable(false);
        m_btnRemoveEnv->Enable(false);
        btnRow->Add(m_btnAddEnv,    0, wxRIGHT, 4);
        btnRow->Add(m_btnEditEnv,   0, wxRIGHT, 4);
        btnRow->Add(m_btnRemoveEnv, 0);
        ps->Add(btnRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 4);

        pw->SetSizer(ps);
        outer->Add(m_envPane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }

    // ---- Footer -------------------------------------------------------------
    outer->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

    if (context == LaunchContext::ProfileOnly) {
        auto* btnSizer = CreateButtonSizer(wxOK | wxCANCEL);
        if (auto* btn = dynamic_cast<wxButton*>(FindWindow(wxID_OK)))
            btn->SetLabel(prefill ? "Save" : "Add");
        outer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 10);
        Bind(wxEVT_BUTTON, &NewConnectionDialog::OnConnectClicked, this, wxID_OK);
    } else {
        auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);
        btnSizer->AddStretchSpacer();
        btnSizer->Add(new wxButton(this, wxID_CANCEL), 0);
        outer->Add(btnSizer, 0, wxEXPAND | wxALL, 10);
    }

    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    // ---- Event bindings -----------------------------------------------------
    Bind(wxEVT_CHOICE,   &NewConnectionDialog::OnAuthChoiceChanged,      this, ID_SSH_AUTH_CHOICE);
    Bind(wxEVT_COMBOBOX, &NewConnectionDialog::OnGeometryChanged,         this, ID_GEOMETRY_COMBO);

    if (m_connectBtn)
        Bind(wxEVT_BUTTON, &NewConnectionDialog::OnConnectClicked, this, ID_BTN_CONNECT);

    Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, &NewConnectionDialog::OnCollapsiblePaneChanged, this);
    Bind(wxEVT_CHOICE, &NewConnectionDialog::OnJumpAuthChoiceChanged, this, ID_JUMP_AUTH_CHOICE);
    Bind(wxEVT_NOTEBOOK_PAGE_CHANGED,   &NewConnectionDialog::OnTabChanged,             this);

    if (m_hostCtrl) {
        Bind(wxEVT_TEXT, &NewConnectionDialog::OnSshFieldChanged, this, m_hostCtrl->GetId());

        m_hostDetectTimer = new wxTimer(this);
        Bind(wxEVT_TIMER, &NewConnectionDialog::OnHostDetectTimer, this, m_hostDetectTimer->GetId());
    }
    if (m_userCtrl)
        Bind(wxEVT_TEXT, &NewConnectionDialog::OnSshFieldChanged, this, m_userCtrl->GetId());

    Bind(wxEVT_BUTTON,  &NewConnectionDialog::OnBrowseWorkingDir, this, ID_BTN_BROWSE_WORKDIR_PTY);
    Bind(wxEVT_BUTTON,  &NewConnectionDialog::OnBrowseWorkingDir, this, ID_BTN_BROWSE_WORKDIR_SSH);

    Bind(wxEVT_BUTTON,  &NewConnectionDialog::OnAddEnvVar,      this, ID_BTN_ADD_ENV);
    Bind(wxEVT_BUTTON,  &NewConnectionDialog::OnEditEnvVar,     this, ID_BTN_EDIT_ENV);
    Bind(wxEVT_BUTTON,  &NewConnectionDialog::OnRemoveEnvVar,   this, ID_BTN_REMOVE_ENV);
    Bind(wxEVT_LISTBOX, &NewConnectionDialog::OnEnvVarSelected, this, ID_LIST_ENV);

    Bind(wxEVT_CHECKBOX, &NewConnectionDialog::OnUseProfileTitleToggled, this, ID_CB_USE_PROFILE_TITLE);

    if (m_profileCombo) {
        Bind(wxEVT_COMBOBOX, &NewConnectionDialog::OnProfileSelected,   this, ID_PROFILE_COMBO);
        Bind(wxEVT_TEXT,     &NewConnectionDialog::OnProfileTextChanged, this, ID_PROFILE_COMBO);
    }

    if (prefill)
        ApplyPrefill(*prefill);

    // Set initial Connect button state after prefill may have set m_selectedProfileId
    UpdateConnectButton();

    // Move focus to the profile field so the user can start typing immediately
    if (m_profileCombo)
        m_profileCombo->SetFocus();
    else if (m_profileNameCtrl)
        m_profileNameCtrl->SetFocus();
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
NewConnectionDialog::~NewConnectionDialog()
{
    // m_hostDetectTimer is heap-allocated (new wxTimer(this)) so it is not
    // destroyed automatically with the dialog. Stop it first so the pending
    // GTK timeout source is removed; then delete it. Without this, a 600 ms
    // debounce that was still counting down when the user clicked Connect would
    // fire after the dialog is freed and dereference the dead owner pointer,
    // causing a segfault in wxTimer::Notify → m_owner->ProcessEvent.
    if (m_hostDetectTimer) {
        m_hostDetectTimer->Stop();
        delete m_hostDetectTimer;
        m_hostDetectTimer = nullptr;
    }
}

// ---------------------------------------------------------------------------
// ApplyPrefill
// ---------------------------------------------------------------------------
void NewConnectionDialog::ApplyPrefill(const term::db::ConnectionProfile& profile)
{
    // Name field
    if (m_profileNameCtrl)
        m_profileNameCtrl->SetValue(profile.name);

    // Transport settings
    std::visit([this](const auto& desc) {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, term::session::PtyDesc>) {
            m_notebook->SetSelection(kTabPty);
            m_shellCtrl->SetValue(desc.shell);
            if (m_ptyCmdCtrl) m_ptyCmdCtrl->SetValue(desc.command);
        } else if constexpr (std::is_same_v<T, term::session::SshDesc>) {
            m_notebook->SetSelection(kTabSsh);
            m_hostCtrl->SetValue(desc.host);
            m_portCtrl->SetValue(wxString::Format("%d", desc.port));
            m_userCtrl->SetValue(desc.username);
            m_timeoutCtrl->SetValue(desc.connectTimeoutSec);
            m_keepaliveCtrl->SetValue(desc.keepaliveSeconds);
            m_remoteCmdCtrl->SetValue(desc.remoteCommand);
            m_cbCompress->SetValue(desc.compress);
            m_cbX11Fwd->SetValue(desc.x11Forwarding);
            m_cbAgentFwd->SetValue(desc.agentForwarding);
            const int sel = [&]() -> int {
                switch (desc.authMethod) {
                    case term::session::SshAuthMethod::Password:       return 1;
                    case term::session::SshAuthMethod::PrivateKey:     return 2;
                    case term::session::SshAuthMethod::KbdInteractive: return 3;
                    default:                                            return 0;
                }
            }();
            m_authChoice->SetSelection(sel);
            m_authBook->SetSelection(static_cast<size_t>(sel));
            if (desc.authMethod == term::session::SshAuthMethod::PrivateKey)
                m_keyPicker->SetPath(desc.privateKeyPath);
            if (desc.authMethod == term::session::SshAuthMethod::Agent)
                m_agentHintPicker->SetPath(desc.agentIdentityHint);
            // Jump host
            if (desc.proxyJump && !desc.proxyJump->host.empty()) {
                m_jumpHostCtrl->SetValue(desc.proxyJump->host);
                m_jumpPortCtrl->SetValue(wxString::Format("%d", desc.proxyJump->port));
                m_jumpUserCtrl->SetValue(desc.proxyJump->user);
                m_jumpHintPicker->SetPath(desc.proxyJump->agentIdentityHint);
                m_jumpKeyPicker->SetPath(desc.proxyJump->privateKeyPath);
                const int jsel = [&]() -> int {
                    switch (desc.proxyJump->authMethod) {
                        case term::session::SshAuthMethod::Password:       return 1;
                        case term::session::SshAuthMethod::PrivateKey:     return 2;
                        case term::session::SshAuthMethod::KbdInteractive: return 3;
                        default:                                            return 0;
                    }
                }();
                m_jumpAuthChoice->SetSelection(jsel);
                m_jumpAuthBook->SetSelection(static_cast<size_t>(jsel));
                m_jumpPane->Expand();
            }
        } else if constexpr (std::is_same_v<T, term::session::SerialDesc>) {
            m_notebook->SetSelection(kTabSerial);
            m_deviceCtrl->SetValue(desc.device);
            m_baudCtrl->SetStringSelection(wxString::Format("%u", desc.baudRate));
            m_dataBitsCtrl->SetStringSelection(wxString::Format("%u", desc.dataBits));
            m_stopBitsCtrl->SetStringSelection(
                desc.stopBits == term::session::SerialStopBits::Two ? "2" : "1");
            switch (desc.parity) {
                case term::session::SerialParity::Even: m_parityCtrl->SetStringSelection("Even"); break;
                case term::session::SerialParity::Odd:  m_parityCtrl->SetStringSelection("Odd");  break;
                default:                                m_parityCtrl->SetStringSelection("None"); break;
            }
            switch (desc.flowControl) {
                case term::session::SerialFlowControl::Hardware:
                    m_flowCtrlCombo->SetStringSelection("Hardware"); break;
                case term::session::SerialFlowControl::Software:
                    m_flowCtrlCombo->SetStringSelection("Software"); break;
                default:
                    m_flowCtrlCombo->SetStringSelection("None"); break;
            }
            m_dialScriptCtrl->SetValue(desc.dialScript);
        } else {
            m_notebook->SetSelection(kTabPty);
        }
    }, profile.transport);

    // Session init — working dir per-tab, env vars in the shared panel
    const auto& si = profile.sessionInit;
    if (m_ptyWorkDirCtrl)  m_ptyWorkDirCtrl->SetValue(si.workingDir);
    if (m_sshWorkDirCtrl)  m_sshWorkDirCtrl->SetValue(si.workingDir);
    if (m_cbPtyLoginShell) m_cbPtyLoginShell->SetValue(si.loginShell);
    if (m_cbSshLoginShell) m_cbSshLoginShell->SetValue(si.loginShell);

    if (m_envFileCtrl && !si.envFilePath.empty())
        m_envFileCtrl->SetPath(si.envFilePath);
    if (m_envVarList) {
        m_envVarList->Clear();
        for (const auto& ev : si.envVars)
            m_envVarList->Append(wxString(ev.key + "=" + ev.value));
    }

    // Profile title
    if (m_profileTitleCtrl)   m_profileTitleCtrl->SetValue(profile.profileTitle);
    if (m_cbUseProfileTitle) {
        m_cbUseProfileTitle->SetValue(profile.useProfileTitle);
        if (m_profileTitleCtrl) m_profileTitleCtrl->Enable(profile.useProfileTitle);
    }

    // Wrap mode
    m_cbWrapMode->SetValue(profile.wrapMode);

    // Geometry — find matching preset or fall back to Custom
    bool matched = false;
    for (int i = 0; i < static_cast<int>(m_geometryPresets.size()); ++i) {
        if (m_geometryPresets[i].cols == profile.columnWidth &&
            m_geometryPresets[i].rows == profile.rows) {
            m_geometryCombo->SetSelection(i);
            m_customGeomPanel->Show(false);
            matched = true;
            break;
        }
    }
    if (!matched) {
        m_geometryCombo->SetSelection(static_cast<int>(m_geometryPresets.size())); // "Custom"
        m_customGeomPanel->Show(true);
        m_colsCtrl->SetValue(profile.columnWidth);
        m_rowsCtrl->SetValue(profile.rows);
        Layout();
        Fit();
        SetMinSize(GetSize());
    }

}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------
void NewConnectionDialog::UpdateConnectButton()
{
    if (!m_connectBtn) return;
    bool enabled = !m_selectedProfileId.empty();
    if (!enabled) {
        const int tab = m_notebook->GetSelection();
        if (tab == kTabPty) {
            enabled = true;
        } else if (tab == kTabSsh) {
            enabled = !m_hostCtrl->GetValue().IsEmpty();
        } else if (tab == kTabSerial) {
            enabled = !m_deviceCtrl->GetValue().IsEmpty();
        }
    }
    m_connectBtn->Enable(enabled);
}

void NewConnectionDialog::OnConnectClicked(wxCommandEvent&)
{
    // Validate: "Save as Profile" requires a name
    if (m_cbSaveProfile && m_cbSaveProfile->GetValue() && GetProfileName().empty()) {
        wxMessageBox("Enter a profile name before saving.", "Save as Profile",
                     wxOK | wxICON_WARNING, this);
        if (m_profileCombo) m_profileCombo->SetFocus();
        return;
    }

    const int tab = m_notebook->GetSelection();

    if (tab == kTabSsh) {
        if (m_hostCtrl->GetValue().IsEmpty()) {
            wxMessageBox("Please enter a hostname.", "SSH Connection",
                         wxOK | wxICON_WARNING, this);
            m_hostCtrl->SetFocus();
            return;
        }
        unsigned long port = 0;
        if (!m_portCtrl->GetValue().ToULong(&port) || port < 1 || port > 65535) {
            wxMessageBox("Port must be a number between 1 and 65535.", "SSH Connection",
                         wxOK | wxICON_WARNING, this);
            m_portCtrl->SetFocus();
            return;
        }
    }
    if (tab == kTabSerial && m_deviceCtrl->GetValue().IsEmpty()) {
        wxMessageBox("Please enter a serial device path (e.g. /dev/ttyUSB0).",
                     "Serial Connection", wxOK | wxICON_WARNING, this);
        m_deviceCtrl->SetFocus();
        return;
    }

    EndModal(wxID_OK);
}

void NewConnectionDialog::OnCollapsiblePaneChanged(wxCollapsiblePaneEvent&)
{
    Layout();
    Fit();
    SetMinSize(GetSize());
}

void NewConnectionDialog::OnJumpAuthChoiceChanged(wxCommandEvent&)
{
    const int sel = m_jumpAuthChoice->GetSelection();
    if (m_jumpAuthBook)
        m_jumpAuthBook->SetSelection(static_cast<size_t>(sel));
    if (m_jumpPane && m_jumpPane->GetPane())
        m_jumpPane->GetPane()->Layout();
    Layout();
    Fit();
    SetMinSize(GetSize());
}

void NewConnectionDialog::OnAuthChoiceChanged(wxCommandEvent&)
{
    const int sel = m_authChoice->GetSelection();
    if (m_authBook)
        m_authBook->SetSelection(static_cast<size_t>(sel));
    if (m_notebook->GetCurrentPage())
        m_notebook->GetCurrentPage()->Layout();
    Layout();
    Fit();
    SetMinSize(GetSize());
}

void NewConnectionDialog::OnTabChanged(wxBookCtrlEvent&)
{
    UpdateConnectButton();
}

void NewConnectionDialog::OnSshFieldChanged(wxCommandEvent& evt)
{
    UpdateConnectButton();

    // Restart the debounce timer whenever the host field changes so we only
    // run ssh -G once the user pauses typing (600 ms of no input).
    if (m_hostDetectTimer && evt.GetEventObject() == m_hostCtrl)
        m_hostDetectTimer->StartOnce(600);
}

void NewConnectionDialog::OnHostDetectTimer(wxTimerEvent&)
{
    DetectProxyJumpFromConfig();
}

void NewConnectionDialog::DetectProxyJumpFromConfig()
{
    // if (!m_jumpPane || !m_jumpPane->IsCollapsed()) return;
    if (!m_jumpPane) return;

    const std::string host = m_hostCtrl->GetValue().Trim().ToStdString();
    if (host.empty()) return;

    const std::string user = m_userCtrl->GetValue().ToStdString();
    unsigned long port = 22;
    m_portCtrl->GetValue().ToULong(&port);

    auto pj = term::transport::QuerySshConfigProxyJump(
        host, static_cast<uint16_t>(port), user);
    if (!pj || pj->host.empty()) return;

    m_jumpHostCtrl->SetValue(pj->host);
    m_jumpPortCtrl->SetValue(wxString::Format("%d", pj->port));
    m_jumpUserCtrl->SetValue(pj->user);

    if (m_jumpHintPicker->GetPath().empty()) {
        const std::string jumpUser = pj->user.empty() ? user : pj->user;
        const auto ids = term::transport::QuerySshConfigIdentities(
            pj->host, static_cast<uint16_t>(pj->port), jumpUser);
        if (!ids.empty())
            m_jumpHintPicker->SetPath(wxString::FromUTF8(ids.front().string()));
    }

    m_jumpPane->Expand();
    Layout();
    Fit();
    SetMinSize(GetSize());
}

void NewConnectionDialog::OnBrowseWorkingDir(wxCommandEvent& evt)
{
    wxDirDialog dlg(this, "Select Working Directory", wxEmptyString,
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    const wxString path = dlg.GetPath();
    if (evt.GetId() == ID_BTN_BROWSE_WORKDIR_PTY && m_ptyWorkDirCtrl)
        m_ptyWorkDirCtrl->SetValue(path);
    else if (evt.GetId() == ID_BTN_BROWSE_WORKDIR_SSH && m_sshWorkDirCtrl)
        m_sshWorkDirCtrl->SetValue(path);
}

void NewConnectionDialog::OnAddEnvVar(wxCommandEvent&)
{
    if (!m_envVarList) return;
    wxString val = wxGetTextFromUser("Enter KEY=VALUE:", "Add Environment Variable",
                                     wxEmptyString, this);
    val.Trim(true).Trim(false);
    if (val.IsEmpty()) return;
    if (val.Find('=') == wxNOT_FOUND || val.BeforeFirst('=').IsEmpty()) {
        wxMessageBox("Format must be KEY=VALUE with a non-empty key.",
                     "Invalid Entry", wxOK | wxICON_WARNING, this);
        return;
    }
    m_envVarList->Append(val);
}

void NewConnectionDialog::OnEditEnvVar(wxCommandEvent&)
{
    if (!m_envVarList) return;
    const int sel = m_envVarList->GetSelection();
    if (sel == wxNOT_FOUND) return;
    wxString val = wxGetTextFromUser("Edit KEY=VALUE:", "Edit Environment Variable",
                                     m_envVarList->GetString(sel), this);
    val.Trim(true).Trim(false);
    if (val.IsEmpty()) return;
    if (val.Find('=') == wxNOT_FOUND || val.BeforeFirst('=').IsEmpty()) {
        wxMessageBox("Format must be KEY=VALUE with a non-empty key.",
                     "Invalid Entry", wxOK | wxICON_WARNING, this);
        return;
    }
    m_envVarList->SetString(sel, val);
}

void NewConnectionDialog::OnRemoveEnvVar(wxCommandEvent&)
{
    if (!m_envVarList) return;
    const int sel = m_envVarList->GetSelection();
    if (sel == wxNOT_FOUND) return;
    m_envVarList->Delete(static_cast<unsigned int>(sel));
    wxCommandEvent dummy;
    OnEnvVarSelected(dummy);
}

void NewConnectionDialog::OnEnvVarSelected(wxCommandEvent&)
{
    const bool hasSelection = m_envVarList && m_envVarList->GetSelection() != wxNOT_FOUND;
    if (m_btnEditEnv)   m_btnEditEnv->Enable(hasSelection);
    if (m_btnRemoveEnv) m_btnRemoveEnv->Enable(hasSelection);
}

void NewConnectionDialog::OnUseProfileTitleToggled(wxCommandEvent&)
{
    if (m_profileTitleCtrl)
        m_profileTitleCtrl->Enable(m_cbUseProfileTitle->GetValue());
}

void NewConnectionDialog::OnGeometryChanged(wxCommandEvent&)
{
    const bool isCustom =
        (m_geometryCombo->GetSelection() == static_cast<int>(m_geometryPresets.size()));
    m_customGeomPanel->Show(isCustom);
    Layout();
    Fit();
    SetMinSize(GetSize());
}

void NewConnectionDialog::OnProfileSelected(wxCommandEvent&)
{
    if (!m_profileCombo) return;
    const wxString sel = m_profileCombo->GetStringSelection();
    for (const auto& p : m_existingProfiles) {
        if (p.name == sel.ToStdString()) {
            m_selectedProfileId = p.id;
            ApplyPrefill(p);
            UpdateConnectButton();
            if (m_connectBtn) m_connectBtn->SetFocus();
            return;
        }
    }
    m_selectedProfileId.clear();
    UpdateConnectButton();
}

void NewConnectionDialog::OnProfileTextChanged(wxCommandEvent&)
{
    // Check for exact profile name match — covers both autocomplete selection and
    // manual typing. wxEVT_COMBOBOX does not fire for GTK autocomplete picks.
    if (m_profileCombo) {
        const std::string text = m_profileCombo->GetValue().Trim().ToStdString();
        for (const auto& p : m_existingProfiles) {
            if (p.name == text) {
                if (m_selectedProfileId != p.id) {
                    m_selectedProfileId = p.id;
                    ApplyPrefill(p);
                    if (m_connectBtn) m_connectBtn->SetFocus();
                }
                UpdateConnectButton();
                return;
            }
        }
    }
    m_selectedProfileId.clear();
    UpdateConnectButton();
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------
static std::pair<unsigned short, unsigned short> resolveGeometry(
        wxComboBox* combo,
        wxSpinCtrl* colsCtrl,
        wxSpinCtrl* rowsCtrl,
        const std::vector<GeometryPreset>& presets)
{
    const int sel = combo->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(presets.size()))
        return {presets[sel].cols, presets[sel].rows};
    // Custom
    return {static_cast<unsigned short>(colsCtrl->GetValue()),
            static_cast<unsigned short>(rowsCtrl->GetValue())};
}

static void collectEnvVars(wxListBox* list,
                            std::vector<term::session::EnvVar>& out)
{
    if (!list) return;
    for (unsigned int i = 0; i < list->GetCount(); ++i) {
        const std::string kv = list->GetString(i).ToStdString();
        const auto eq = kv.find('=');
        if (eq != std::string::npos)
            out.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
    }
}

ConnectionParams NewConnectionDialog::GetParams() const
{
    const bool wrapMode = m_cbWrapMode->GetValue();
    auto [cols, rows] = resolveGeometry(m_geometryCombo, m_colsCtrl, m_rowsCtrl, m_geometryPresets);

    const std::string envFilePath = m_envFileCtrl ? m_envFileCtrl->GetPath().ToStdString() : std::string{};
    std::vector<term::session::EnvVar> envVars;
    collectEnvVars(m_envVarList, envVars);

    const int tab = m_notebook->GetSelection();

    if (tab == kTabPty) {
        PtyParams p;
        p.shell       = m_shellCtrl->GetValue().ToStdString();
        p.command     = m_ptyCmdCtrl ? m_ptyCmdCtrl->GetValue().ToStdString() : std::string{};
        p.wrapMode    = wrapMode;
        p.columnWidth = cols;
        p.rows        = rows;
        if (m_ptyWorkDirCtrl)  p.workingDir  = m_ptyWorkDirCtrl->GetValue().ToStdString();
        if (m_cbPtyLoginShell) p.loginShell  = m_cbPtyLoginShell->GetValue();
        p.envFilePath = envFilePath;
        p.envVars     = envVars;
        if (m_profileTitleCtrl)   p.profileTitle    = m_profileTitleCtrl->GetValue().ToStdString();
        if (m_cbUseProfileTitle)  p.useProfileTitle = m_cbUseProfileTitle->GetValue();
        return p;
    }

    if (tab == kTabSsh) {
        SshParams p;
        p.host              = m_hostCtrl->GetValue().ToStdString();
        p.username          = m_userCtrl->GetValue().ToStdString();
        if (p.username.empty()) {
            const char* u = std::getenv("USER");
            if (!u || *u == '\0') u = std::getenv("LOGNAME");
            if (u) p.username = u;
        }
        p.connectTimeoutSec = m_timeoutCtrl->GetValue();
        p.keepaliveSeconds  = m_keepaliveCtrl->GetValue();
        p.remoteCommand     = m_remoteCmdCtrl->GetValue().ToStdString();
        p.compress          = m_cbCompress->GetValue();
        p.x11Forwarding     = m_cbX11Fwd->GetValue();
        p.agentForwarding   = m_cbAgentFwd->GetValue();
        p.wrapMode    = wrapMode;
        p.columnWidth = cols;
        p.rows        = rows;
        if (m_sshWorkDirCtrl)    p.workingDir  = m_sshWorkDirCtrl->GetValue().ToStdString();
        if (m_cbSshLoginShell)   p.loginShell  = m_cbSshLoginShell->GetValue();
        p.envFilePath = envFilePath;
        p.envVars     = envVars;
        if (m_profileTitleCtrl)  p.profileTitle    = m_profileTitleCtrl->GetValue().ToStdString();
        if (m_cbUseProfileTitle) p.useProfileTitle = m_cbUseProfileTitle->GetValue();

        // Parse port — validated in OnConnectClicked; clamp here as a safety net
        unsigned long port = 22;
        m_portCtrl->GetValue().ToULong(&port);
        p.port = static_cast<unsigned short>(std::clamp(port, 1UL, 65535UL));

        switch (m_authChoice->GetSelection()) {
            case 1:
                p.authMethod = SshAuthChoice::Password;
                p.password   = m_passCtrl->GetValue().ToStdString();
                break;
            case 2:
                p.authMethod     = SshAuthChoice::PrivateKey;
                p.privateKeyPath = m_keyPicker->GetPath().ToStdString();
                p.passphrase     = m_passphraseCtrl->GetValue().ToStdString();
                break;
            case 3:
                p.authMethod = SshAuthChoice::KeyboardInteractive;
                break;
            default:
                p.authMethod        = SshAuthChoice::Agent;
                p.agentIdentityHint = m_agentHintPicker->GetPath().ToStdString();
                break;
        }

        // Jump host — populated only when the pane is expanded and host is non-empty
        if (m_jumpPane && !m_jumpPane->IsCollapsed()) {
            const std::string jhost = m_jumpHostCtrl->GetValue().ToStdString();
            if (!jhost.empty()) {
                SshParams::ProxyJumpParams pj;
                pj.host = jhost;
                unsigned long jport = 22;
                m_jumpPortCtrl->GetValue().ToULong(&jport);
                pj.port = static_cast<unsigned short>(std::clamp(jport, 1UL, 65535UL));
                pj.user = m_jumpUserCtrl->GetValue().ToStdString();
                switch (m_jumpAuthChoice->GetSelection()) {
                    case 1: pj.authMethod = SshAuthChoice::Password;
                            pj.password   = m_jumpPassCtrl->GetValue().ToStdString();
                            break;
                    case 2: pj.authMethod    = SshAuthChoice::PrivateKey;
                            pj.privateKeyPath = m_jumpKeyPicker->GetPath().ToStdString();
                            pj.passphrase     = m_jumpPassphraseCtrl->GetValue().ToStdString();
                            break;
                    case 3: pj.authMethod = SshAuthChoice::KeyboardInteractive;
                            break;
                    default: pj.authMethod        = SshAuthChoice::Agent;
                             pj.agentIdentityHint = m_jumpHintPicker->GetPath().ToStdString();
                             break;
                }
                p.proxyJump = std::move(pj);
            }
        }
        return p;
    }

    if (tab == kTabSerial) {
        SerialParams p;
        p.device = m_deviceCtrl->GetValue().ToStdString();
        unsigned long baud = 115200;
        m_baudCtrl->GetStringSelection().ToULong(&baud);
        p.baudRate = static_cast<unsigned int>(baud);
        unsigned long db = 8;
        m_dataBitsCtrl->GetStringSelection().ToULong(&db);
        p.dataBits    = static_cast<unsigned short>(db);
        p.stopBits    = m_stopBitsCtrl->GetStringSelection().ToStdString();
        p.parity      = m_parityCtrl->GetStringSelection().ToStdString();
        p.flowControl = m_flowCtrlCombo->GetStringSelection().ToStdString();
        p.dialScript  = m_dialScriptCtrl->GetValue().ToStdString();
        p.wrapMode    = wrapMode;
        p.columnWidth = cols;
        p.rows        = rows;
        p.envFilePath = envFilePath;
        p.envVars     = envVars;
        if (m_profileTitleCtrl)  p.profileTitle    = m_profileTitleCtrl->GetValue().ToStdString();
        if (m_cbUseProfileTitle) p.useProfileTitle = m_cbUseProfileTitle->GetValue();
        return p;
    }

    // Fallback (should not be reached)
    return PtyParams{};
}

std::string NewConnectionDialog::GetProfileName() const
{
    if (m_profileCombo)
        return m_profileCombo->GetValue().Trim().ToStdString();
    if (m_profileNameCtrl)
        return m_profileNameCtrl->GetValue().Trim().ToStdString();
    return {};
}

bool NewConnectionDialog::GetSaveAsProfile() const
{
    return m_cbSaveProfile && m_cbSaveProfile->GetValue();
}

std::string NewConnectionDialog::GetSelectedProfileId() const
{
    return m_selectedProfileId;
}

LaunchPlacement NewConnectionDialog::GetLaunchPlacement() const
{
    if (!m_placementCtrl) return LaunchPlacement::ActiveTile;
    switch (m_placementCtrl->GetSelection()) {
        case 1: return LaunchPlacement::NewTile;
        case 2: return LaunchPlacement::NewWindow;
        default: return LaunchPlacement::ActiveTile;
    }
}

} // namespace ui
