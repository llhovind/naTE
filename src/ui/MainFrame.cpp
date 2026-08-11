#include "ui/MainFrame.h"
#include "ui/AboutDialog.h"
#include "ui/ConnectionFactory.h"
#include "ui/ConnectionManagerDialog.h"
#include "ui/DialogPlacement.h"
#include "ui/GeometryDialog.h"
#include "ui/PreferencesDialog.h"
#include "ui/TerminalTile.h"
#include "config/ColorScheme.h"
#include "ui/WorkspaceManagerDialog.h"
#include "ui/AppIconBundle.h"
#include "ui/UIManager.h"
#include "db/ConnectionProfile.h"
#include "db/ConnectionStore.h"
#include "app/App.h"
#include <wx/fontdlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/textdlg.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
    constexpr int ID_NEW_CONNECTION          = wxID_HIGHEST + 1;
    constexpr int ID_TOGGLE_wrapMode         = wxID_HIGHEST + 2;
    constexpr int ID_CONNECTION_MANAGER      = wxID_HIGHEST + 3;
    constexpr int ID_NEW_WINDOW              = wxID_HIGHEST + 4;
    constexpr int ID_QUIT_ALL                = wxID_HIGHEST + 5;
    constexpr int ID_BROADCAST_MODE          = wxID_HIGHEST + 6;
    constexpr int ID_NEW_CONNECTION_IN_TILE  = wxID_HIGHEST + 7;
    constexpr int ID_SET_GEOMETRY_CUSTOM     = wxID_HIGHEST + 19;
    constexpr int ID_SET_FONT                = wxID_HIGHEST + 20;
    constexpr int ID_SAVE_SESSION_FILE_TERM  = wxID_HIGHEST + 21;
    constexpr int ID_OPEN_IN_NEW_TILE        = wxID_HIGHEST + 22;
    constexpr int ID_OPEN_IN_NEW_WINDOW_TERM = wxID_HIGHEST + 23;
    constexpr int ID_RESET_TERMINAL          = wxID_HIGHEST + 24;
    constexpr int ID_RESET_AND_CLEAR         = wxID_HIGHEST + 25;
    constexpr int ID_TRANSFER_FILES          = wxID_HIGHEST + 26;
    constexpr int ID_RESTORE_WORKSPACE       = wxID_HIGHEST + 28;
    constexpr int ID_SAVE_AS_WORKSPACE       = wxID_HIGHEST + 29;
    constexpr int ID_OPEN_WORKSPACE          = wxID_HIGHEST + 30;
    constexpr int ID_ABOUT                   = wxID_HIGHEST + 31;
    constexpr int ID_PREFERENCES             = wxID_HIGHEST + 32;
    constexpr int ID_CLOSE_ALL_SESSIONS      = wxID_HIGHEST + 33;
    constexpr int ID_REFIT_WINDOW            = wxID_HIGHEST + 34;
    constexpr int ID_TILE_LAYOUT_ROW_FIRST   = wxID_HIGHEST + 35;
    constexpr int ID_TILE_LAYOUT_COL_FIRST   = wxID_HIGHEST + 36;
    constexpr int ID_EDIT_REMOTE_FILE        = wxID_HIGHEST + 37;
    constexpr int ID_FILE_EXPLORER           = wxID_HIGHEST + 38;

    static bool IsValidWorkspaceName(const std::string& s)
    {
        if (s.empty() || s.size() > 64) return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == ' ';
        });
    }

    // Window menu: window entries occupy [kWindowMenuBase, kWindowMenuBase + kWindowMenuMax).
    constexpr int kWindowMenuBase    = wxID_HIGHEST + 400;
    constexpr int kWindowMenuMax     = 64;
    // Window menu: session entries occupy [kWindowSessionBase, kWindowSessionBase + kWindowSessionMax).
    constexpr int kWindowSessionBase = wxID_HIGHEST + 500;
    constexpr int kWindowSessionMax  = 128;

    // Set Geometry submenu: one entry per configured preset, indexed by
    // (id - kGeometryPresetBase) into AppConfig::geometryPresets.
    // Must stay clear of the Window session range above
    // ([kWindowSessionBase, kWindowSessionBase + kWindowSessionMax) = [+500, +628)).
    constexpr int kGeometryPresetBase = wxID_HIGHEST + 800;
    constexpr int kGeometryPresetMax  = 64;

}

MainFrame::MainFrame(const AppConfig& cfg,
                     term::input::InputRouter& router,
                     term::db::ConnectionStore& store)
    : wxFrame(nullptr, wxID_ANY, "naTE"),
      m_router(router),
      m_store(store),
      m_cfg(cfg)
{
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    SetIcons(ui::AppIconBundle());

    // ---- Edit menu (populated by UIManager) ----------------------------------
    m_editMenu = new wxMenu;

    // ---- Connection menu -----------------------------------------------------
    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_NEW_CONNECTION,         "New Connection\tCtrl+Shift+N");
    m_connMenu->Append(ID_NEW_CONNECTION_IN_TILE, "New Connection in Tab\tCtrl+Shift+T");
    m_connMenu->Append(ID_CONNECTION_MANAGER,     "Connection Manager...\tCtrl+Shift+M");
    m_connMenu->AppendSeparator();
    m_connMenu->Append(ID_RESTORE_WORKSPACE,      "Restore Previous Workspace\tCtrl+Shift+R");
    m_connMenu->Append(ID_SAVE_AS_WORKSPACE,      "Save Workspace As...");
    m_connMenu->Append(ID_OPEN_WORKSPACE,         "Open Saved Workspaces...");
    m_connMenu->AppendSeparator();
    m_connMenu->Append(ID_CLOSE_ALL_SESSIONS,     "Close All Sessions");
    m_connMenu->Append(wxID_CLOSE,                "Close This Window\tCtrl+Shift+Q");
    m_connMenu->Append(ID_QUIT_ALL,               "Close All and Exit\tCtrl+Shift+X");

    Bind(wxEVT_MENU, &MainFrame::OnNewConnection,             this, ID_NEW_CONNECTION);
    Bind(wxEVT_MENU, &MainFrame::OnNewConnectionInActiveTile, this, ID_NEW_CONNECTION_IN_TILE);
    Bind(wxEVT_MENU, &MainFrame::OnConnectionManager,         this, ID_CONNECTION_MANAGER);
    Bind(wxEVT_MENU, &MainFrame::OnRestoreWorkspace,          this, ID_RESTORE_WORKSPACE);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(static_cast<App*>(wxTheApp)->HasRestoreState());
    }, ID_RESTORE_WORKSPACE);
    Bind(wxEVT_MENU, &MainFrame::OnSaveAsWorkspace,           this, ID_SAVE_AS_WORKSPACE);
    Bind(wxEVT_MENU, &MainFrame::OnOpenWorkspace,             this, ID_OPEN_WORKSPACE);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(static_cast<App*>(wxTheApp)->HasNamedWorkspaces());
    }, ID_OPEN_WORKSPACE);
    Bind(wxEVT_MENU, &MainFrame::OnCloseAllSessions,          this, ID_CLOSE_ALL_SESSIONS);
    Bind(wxEVT_MENU, &MainFrame::OnCloseThisWindow,           this, wxID_CLOSE);
    Bind(wxEVT_MENU, &MainFrame::OnQuitAll,                   this, ID_QUIT_ALL);

    // ---- Terminal menu -------------------------------------------------------
    auto* termMenu = new wxMenu;
    m_miwrapMode  = termMenu->AppendCheckItem(ID_TOGGLE_wrapMode, "Toggle Wrap Mode\tCtrl+Shift+W");
    m_miBroadcast = termMenu->AppendCheckItem(ID_BROADCAST_MODE,  "Broadcast Input Mode\tCtrl+Shift+B");
    Bind(wxEVT_MENU, &MainFrame::OnTogglewrapMode,  this, ID_TOGGLE_wrapMode);
    Bind(wxEVT_MENU, &MainFrame::OnToggleBroadcast, this, ID_BROADCAST_MODE);

    termMenu->AppendSeparator();
    m_geoMenu = new wxMenu;
    BuildGeometryMenu();
    termMenu->AppendSubMenu(m_geoMenu, "Set Geometry");
    termMenu->Append(ID_SET_FONT, "Set Font...");
    Bind(wxEVT_MENU, &MainFrame::OnSetGeometryPreset, this,
         kGeometryPresetBase, kGeometryPresetBase + kGeometryPresetMax - 1);
    Bind(wxEVT_MENU, &MainFrame::OnSetGeometryCustom, this, ID_SET_GEOMETRY_CUSTOM);
    Bind(wxEVT_MENU, &MainFrame::OnSetFont,           this, ID_SET_FONT);

    termMenu->AppendSeparator();
    termMenu->Append(ID_RESET_TERMINAL,  "Reset Terminal");
    termMenu->Append(ID_RESET_AND_CLEAR, "Reset and Clear...");
    Bind(wxEVT_MENU, &MainFrame::OnResetTerminal,  this, ID_RESET_TERMINAL);
    Bind(wxEVT_MENU, &MainFrame::OnResetAndClear,  this, ID_RESET_AND_CLEAR);

    termMenu->AppendSeparator();
    termMenu->Append(ID_SAVE_SESSION_FILE_TERM, "Save Session to File...");
    termMenu->AppendSeparator();
    termMenu->Append(ID_TRANSFER_FILES,          "Transfer Files...");
    termMenu->Append(ID_EDIT_REMOTE_FILE,        "Edit Remote File...");
    termMenu->Append(ID_FILE_EXPLORER,           "File Explorer...");
    Bind(wxEVT_MENU, &MainFrame::OnSaveSessionFileTerminal, this, ID_SAVE_SESSION_FILE_TERM);
    Bind(wxEVT_MENU, &MainFrame::OnTransferFiles,           this, ID_TRANSFER_FILES);
    Bind(wxEVT_MENU, &MainFrame::OnEditRemoteFile,          this, ID_EDIT_REMOTE_FILE);
    Bind(wxEVT_MENU, &MainFrame::OnFileExplorer,            this, ID_FILE_EXPLORER);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        // The active session's, not any session's: both items now open the
        // explorer for *this* session, which must have a filesystem or the
        // window would only be able to say so in a message box.
        e.Enable(m_uiManager && m_uiManager->ActiveSessionSupportsFileTransfer());
    }, ID_TRANSFER_FILES);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(m_uiManager && m_uiManager->ActiveSessionSupportsFileTransfer());
    }, ID_EDIT_REMOTE_FILE);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(m_uiManager && m_uiManager->ActiveSessionSupportsFileTransfer());
    }, ID_FILE_EXPLORER);

    termMenu->AppendSeparator();
    termMenu->Append(ID_OPEN_IN_NEW_TILE,        "Move to New Tile");
    termMenu->Append(ID_OPEN_IN_NEW_WINDOW_TERM, "Move to New Window");
    Bind(wxEVT_MENU, &MainFrame::OnOpenInNewTile,        this, ID_OPEN_IN_NEW_TILE);
    Bind(wxEVT_MENU, &MainFrame::OnOpenInNewWindowTerminal, this, ID_OPEN_IN_NEW_WINDOW_TERM);

    // ---- Window menu (New Window + Refit + Tile Layout static; rest populated dynamically) --
    m_windowMenu = new wxMenu;
    m_windowMenu->Append(ID_NEW_WINDOW,   "New Window");
    m_windowMenu->Append(ID_REFIT_WINDOW, "Refit Window to Tiles");
    {
        auto* layoutMenu = new wxMenu;
        layoutMenu->AppendRadioItem(ID_TILE_LAYOUT_ROW_FIRST, "Row Ordered");
        layoutMenu->AppendRadioItem(ID_TILE_LAYOUT_COL_FIRST, "Column Ordered");
        m_windowMenu->AppendSubMenu(layoutMenu, "Tile Layout");
    }
    m_windowMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnNewWindow,            this, ID_NEW_WINDOW);
    Bind(wxEVT_MENU, &MainFrame::OnRefitWindow,          this, ID_REFIT_WINDOW);
    Bind(wxEVT_MENU, &MainFrame::OnTileLayoutRowFirst,   this, ID_TILE_LAYOUT_ROW_FIRST);
    Bind(wxEVT_MENU, &MainFrame::OnTileLayoutColumnFirst,this, ID_TILE_LAYOUT_COL_FIRST);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(m_uiManager && m_uiManager->HasAnySessions());
    }, ID_REFIT_WINDOW);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Check(!m_uiManager || m_uiManager->GetTileLayout() == TileLayout::RowFirst);
    }, ID_TILE_LAYOUT_ROW_FIRST);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Check(m_uiManager && m_uiManager->GetTileLayout() == TileLayout::ColumnFirst);
    }, ID_TILE_LAYOUT_COL_FIRST);
    Bind(wxEVT_MENU, &MainFrame::OnWindowSessionMenuItem, this,
         kWindowSessionBase, kWindowSessionBase + kWindowSessionMax - 1);

    auto* helpMenu = new wxMenu;
    helpMenu->Append(ID_ABOUT, "&About naTE...");
    Bind(wxEVT_MENU, &MainFrame::OnAbout, this, ID_ABOUT);

    auto* menuBar = new wxMenuBar;
    menuBar->Append(m_connMenu,   "&Connection");
    menuBar->Append(m_editMenu,   "&Edit");
    menuBar->Append(termMenu,     "&Terminal");
    menuBar->Append(m_windowMenu, "&Window");
    menuBar->Append(helpMenu,     "&Help");
    SetMenuBar(menuBar);

    SetBackgroundColour(toWx(cfg.uiColors.frameBackground));
    SetSizer(new wxBoxSizer(wxVERTICAL));
    SetClientSize(wxSize(1200, 700));
}

void MainFrame::SetUIManager(ui::UIManager* ui)
{
    m_uiManager = ui;
    m_editMenu->AppendSeparator();
    m_editMenu->Append(ID_PREFERENCES, "&Preferences...");
    Bind(wxEVT_MENU, &MainFrame::OnPreferences, this, ID_PREFERENCES);
}

void MainFrame::OnPreferences(wxCommandEvent&)
{
    auto* app = static_cast<App*>(wxTheApp);
    const auto themes = ColorScheme::scanDirectory(app->GetThemesDir());
    PreferencesDialog dlg(this, m_cfg, themes);
    if (dlg.ShowModal() != wxID_OK) return;
    // ApplyPreferences saves to disk and propagates to all windows via UpdateConfig.
    app->ApplyPreferences(dlg.GetResult());
    wxMessageBox(
        "Preferences saved.\n\n"
        "Appearance changes have been applied to all open terminals.\n"
        "Scrollback size and PTY line width take effect on new connections.",
        "Preferences", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    const int n = m_uiManager ? static_cast<int>(m_uiManager->GetSessionList().size()) : 0;
    if (!wxGetApp().IsSessionManagerShutdown() && event.CanVeto() && n > 1) {
        const wxString heading = wxString::Format(
            "Closing this window will end %d sessions.", n);
        if (!wxGetApp().ConfirmClose(this, "Confirm Close", heading, true)) {
            event.Veto();
            return;
        }
    }
    if (m_uiManager) {
        m_uiManager->FireBeforeClose();
        m_uiManager->CloseAllSessions();
    }
    event.Skip();
}

void MainFrame::OnCloseAllSessions(wxCommandEvent&)
{
    static_cast<App*>(wxTheApp)->CloseAllSessionsGlobal(this);
}

void MainFrame::OnCloseThisWindow(wxCommandEvent&)
{
    Close(false);
}

void MainFrame::OnQuitAll(wxCommandEvent&)
{
    CallAfter([]() { static_cast<App*>(wxTheApp)->QuitAll(); });
}

void MainFrame::OnNewWindow(wxCommandEvent&)
{
    static_cast<App*>(wxTheApp)->CreateNewWindow();
}

void MainFrame::LaunchSession(const term::session::Connection& conn)
{
    static_cast<App*>(wxTheApp)->CreateSessionInWindow(conn, this);
}

void MainFrame::LaunchSessionInTile(const term::session::Connection& conn,
                                    TerminalTile* targetTile)
{
    if (!targetTile) {
        LaunchSession(conn);
        return;
    }
    static_cast<App*>(wxTheApp)->CreateSessionInTile(conn, this, targetTile);
}

void MainFrame::LaunchNewConnectionInTile(TerminalTile* targetTile)
{
    RunConnectionDialog(ui::LaunchContext::ActiveTile, targetTile);
}

bool MainFrame::RunConnectionDialog(ui::LaunchContext context,
                                    TerminalTile* targetTile)
{
    const std::string defaultShell = [&] {
        if (!m_cfg.defaultShell.empty()) return m_cfg.defaultShell;
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/sh");
    }();

    ui::NewConnectionDialog dlg(this, defaultShell, m_cfg.defaultWorkingDir,
                                m_cfg.defaultWrapMode,
                                m_cfg.defaultLoginShell,
                                m_cfg.geometryPresets,
                                m_store.GetAll(),
                                context);
    if (dlg.ShowModal() != wxID_OK)
        return false;

    term::session::Connection conn = ui::ToConnection(dlg.GetParams(), ++m_sessionCount);

    const std::string profileName = dlg.GetProfileName();
    if (dlg.GetSaveAsProfile() && !profileName.empty()) {
        conn.label = profileName;
        ui::SaveProfile(m_store, conn, profileName, dlg.GetSelectedProfileId());
    } else if (!profileName.empty()) {
        conn.label = profileName;
    }

    // Launch per placement choice
    switch (dlg.GetLaunchPlacement()) {
        case ui::LaunchPlacement::NewWindow:
            static_cast<App*>(wxTheApp)->CreateNewWindow()->LaunchSession(conn);
            break;
        case ui::LaunchPlacement::NewTile:
            LaunchSession(conn);
            break;
        default: {  // ActiveTile
            TerminalTile* tile = targetTile
                ? targetTile
                : (m_uiManager ? m_uiManager->GetActiveTile() : nullptr);
            LaunchSessionInTile(conn, tile);
            break;
        }
    }
    return true;
}

void MainFrame::OnNewConnection(wxCommandEvent&)
{
    const bool hasTiles = m_uiManager && m_uiManager->GetActiveTile() != nullptr;
    const auto ctx = hasTiles ? ui::LaunchContext::UserChoice
                              : ui::LaunchContext::ActiveTile;
    RunConnectionDialog(ctx);
}

void MainFrame::OnNewConnectionInActiveTile(wxCommandEvent&)
{
    RunConnectionDialog(ui::LaunchContext::ActiveTile);
}

void MainFrame::OnConnectionManager(wxCommandEvent&)
{
    ui::ConnectionManagerDialog dlg(this, m_store, m_cfg,
        [this](const term::session::Connection& conn, bool openInNewWindow) {
            ++m_sessionCount;
            if (openInNewWindow) {
                auto* f = static_cast<App*>(wxTheApp)->CreateNewWindow();
                f->LaunchSession(conn);
            } else {
                LaunchSession(conn);
            }
        });
    dlg.ShowModal();
}

void MainFrame::OnRestoreWorkspace(wxCommandEvent&)
{
    static_cast<App*>(wxTheApp)->RestoreWorkspaceFromMenu(this);
}

void MainFrame::OnSaveAsWorkspace(wxCommandEvent&)
{
    wxTextEntryDialog dlg(this, "Workspace name:", "Save Workspace");
    if (dlg.ShowModal() != wxID_OK) return;

    const std::string name = dlg.GetValue().Strip(wxString::both).ToStdString();
    if (!IsValidWorkspaceName(name)) {
        wxMessageBox(
            "Name must be 1-64 characters: letters, digits, spaces, hyphens, underscores.",
            "Invalid Name", wxOK | wxICON_WARNING, this);
        return;
    }

    auto* app = static_cast<App*>(wxTheApp);
    if (app->HasNamedWorkspace(name)) {
        if (wxMessageBox("Overwrite existing workspace '" + name + "'?",
                         "Confirm", wxYES_NO | wxICON_QUESTION, this) != wxYES)
            return;
    }
    app->SaveNamedWorkspace(name);
}

void MainFrame::OnOpenWorkspace(wxCommandEvent&)
{
    auto* app = static_cast<App*>(wxTheApp);
    const auto names = app->GetNamedWorkspaceNames();
    if (names.empty()) return;

    ui::WorkspaceManagerDialog dlg(this, names);
    const int result = dlg.ShowModal();

    for (const auto& n : dlg.GetDeletedNames())
        app->DeleteNamedWorkspace(n);

    if (result == wxID_OK)
        app->RestoreNamedWorkspace(dlg.GetSelectedName(), this);
}

void MainFrame::OnTogglewrapMode(wxCommandEvent&)
{
    if (m_uiManager)
        m_uiManager->ToggleWrapModeForActive();
}

void MainFrame::SyncwrapModeMenuItem(bool checked)
{
    if (m_miwrapMode)
        m_miwrapMode->Check(checked);
}

void MainFrame::OnToggleBroadcast(wxCommandEvent&)
{
    if (m_uiManager)
        m_uiManager->ToggleBroadcastMode();
}

void MainFrame::SyncBroadcastMenuItem(bool checked)
{
    if (m_miBroadcast)
        m_miBroadcast->Check(checked);
}

void MainFrame::RebuildWindowMenu(const std::vector<WindowMenuEntry>& entries)
{
    // Unbind all dynamic window items.
    for (int i = 0; i < static_cast<int>(m_windowFrames.size()); ++i)
        Unbind(wxEVT_MENU, &MainFrame::OnWindowMenuItem, this, kWindowMenuBase + i);

    // Remove all items after the 4 static entries: "New Window", "Refit Window to Tiles",
    // "Tile Layout" submenu, and separator (positions 0, 1, 2, 3).
    while (m_windowMenu->GetMenuItemCount() > 4)
        m_windowMenu->Delete(m_windowMenu->FindItemByPosition(4));

    m_windowFrames.clear();
    m_windowSessions.clear();

    int windowId  = kWindowMenuBase;
    int sessionId = kWindowSessionBase;

    for (const auto& entry : entries) {
        const wxString label = entry.title.empty() ? "naTE" : entry.title;
        const wxString prefixed = entry.frame == this ? "* " + label : "  " + label;
        m_windowMenu->Append(windowId, prefixed);
        Bind(wxEVT_MENU, &MainFrame::OnWindowMenuItem, this, windowId);
        m_windowFrames.push_back(entry.frame);
        ++windowId;

        for (size_t si = 0; si < entry.sessions.size(); ++si) {
            const auto& sess = entry.sessions[si];
            const wxString branch = wxString::FromUTF8(
                si + 1 < entry.sessions.size() ? "  \xe2\x94\x9c " : "  \xe2\x94\x94 ");
            m_windowMenu->Append(sessionId, branch + sess.label);
            m_windowSessions.push_back({entry.frame, sess.id});
            ++sessionId;
        }
    }
}

void MainFrame::ActivateSession(term::session::SessionId id)
{
    if (m_uiManager)
        m_uiManager->RequestActivate(id);
}

void MainFrame::BuildGeometryMenu()
{
    if (!m_geoMenu) return;

    // Clear any previously built items (Destroy invalidates each pointer, so walk
    // a snapshot rather than mutating the live item list while iterating).
    while (m_geoMenu->GetMenuItemCount() > 0)
        m_geoMenu->Destroy(m_geoMenu->FindItemByPosition(0));

    const auto& presets = m_cfg.geometryPresets;
    const std::size_t count = std::min<std::size_t>(presets.size(), kGeometryPresetMax);
    for (std::size_t i = 0; i < count; ++i) {
        m_geoMenu->Append(kGeometryPresetBase + static_cast<int>(i),
                          wxString::Format("%u x %u", presets[i].cols, presets[i].rows));
    }

    m_geoMenu->AppendSeparator();
    m_geoMenu->Append(ID_SET_GEOMETRY_CUSTOM, "Custom...");
}

void MainFrame::OnSetGeometryPreset(wxCommandEvent& evt)
{
    if (!m_uiManager) return;
    const std::size_t idx = static_cast<std::size_t>(evt.GetId() - kGeometryPresetBase);
    if (idx >= m_cfg.geometryPresets.size()) return;
    const auto& preset = m_cfg.geometryPresets[idx];
    m_uiManager->SetGeometryForActive(preset.cols, preset.rows);
}

void MainFrame::OnSetGeometryCustom(wxCommandEvent&)
{
    if (!m_uiManager) return;
    const auto current  = m_uiManager->GetActiveGeometry();
    const auto defCols  = current ? current->cols : static_cast<unsigned short>(80);
    const auto defRows  = current ? current->rows : static_cast<unsigned short>(24);
    GeometryDialog dlg(this, defCols, defRows);
    ui::CentreDialogOnTile(dlg, m_uiManager->GetActiveTile());
    if (dlg.ShowModal() == wxID_OK)
        m_uiManager->SetGeometryForActive(dlg.GetCols(), dlg.GetRows());
}
void MainFrame::OnSetFont(wxCommandEvent&)
{
    wxFontData data;
    data.SetAllowSymbols(false);
    data.SetInitialFont(wxFont(m_cfg.fontSize,
                               wxFONTFAMILY_TELETYPE,
                               wxFONTSTYLE_NORMAL,
                               wxFONTWEIGHT_NORMAL,
                               false,
                               wxString::FromUTF8(m_cfg.fontFamily)));
    wxFontDialog dlg(this, data);
    if (dlg.ShowModal() != wxID_OK) return;

    const wxFont selected = dlg.GetFontData().GetChosenFont();
    AppConfig newCfg      = m_cfg;
    newCfg.fontFamily     = selected.GetFaceName().ToStdString();
    newCfg.fontSize       = selected.GetPointSize();
    static_cast<App*>(wxTheApp)->ApplyPreferences(newCfg);
}
void MainFrame::OnResetTerminal(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->ResetActiveTerminal();
}
void MainFrame::OnResetAndClear(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->ResetAndClearActiveTerminal();
}
void MainFrame::OnSaveSessionFileTerminal(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->SaveActiveSessionToFile();
}

void MainFrame::OnTransferFiles(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->OpenFileExplorerForActive(FileExplorerMode::Transfer);
}

void MainFrame::OnEditRemoteFile(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->EditRemoteFileForActive();
}

void MainFrame::OnFileExplorer(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->OpenFileExplorerForActive(FileExplorerMode::Explore);
}

void MainFrame::OnOpenInNewTile(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->MoveActiveSessionToNewTile();
}

void MainFrame::OnOpenInNewWindowTerminal(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->MoveActiveSessionToNewWindow();
}

void MainFrame::OnRefitWindow(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->ResizeFrameToFitTiles();
}

void MainFrame::OnTileLayoutRowFirst(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->SetTileLayout(TileLayout::RowFirst);
}

void MainFrame::OnTileLayoutColumnFirst(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->SetTileLayout(TileLayout::ColumnFirst);
}

void MainFrame::OnWindowMenuItem(wxCommandEvent& evt)
{
    const int idx = evt.GetId() - kWindowMenuBase;
    if (idx >= 0 && idx < static_cast<int>(m_windowFrames.size()))
        m_windowFrames[idx]->Raise();
}

void MainFrame::OnWindowSessionMenuItem(wxCommandEvent& evt)
{
    const int idx = evt.GetId() - kWindowSessionBase;
    if (idx >= 0 && idx < static_cast<int>(m_windowSessions.size())) {
        auto [ownerFrame, id] = m_windowSessions[idx];
        ownerFrame->Raise();
        ownerFrame->ActivateSession(id);
    }
}

void MainFrame::OnAbout(wxCommandEvent&)
{
    AboutDialog dlg(this);
    dlg.ShowModal();
}

bool MainFrame::DropSession(std::span<const term::session::SessionId> ids,
                            ui::DragIntent /*intent*/,
                            wxPoint /*screenPt*/)
{
    return static_cast<App&>(wxGetApp()).DropSession(ids, this, nullptr);
}
