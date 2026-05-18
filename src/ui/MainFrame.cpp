#include "ui/MainFrame.h"
#include "ui/AboutDialog.h"
#include "ui/ConnectionFactory.h"
#include "ui/ConnectionManagerDialog.h"
#include "ui/GeometryDialog.h"
#include "ui/SnapshotManagerDialog.h"
#include "ui/UIManager.h"
#include "db/ConnectionProfile.h"
#include "db/ConnectionStore.h"
#include "app/App.h"
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
    constexpr int ID_CLOSE_ACTIVE_SESSION    = wxID_HIGHEST + 8;
    constexpr int ID_SET_GEOMETRY_80x24      = wxID_HIGHEST + 9;
    constexpr int ID_SET_GEOMETRY_132x24     = wxID_HIGHEST + 18;
    constexpr int ID_SET_GEOMETRY_CUSTOM     = wxID_HIGHEST + 19;
    constexpr int ID_SET_FONT                = wxID_HIGHEST + 20;
    constexpr int ID_SAVE_SESSION_FILE_TERM  = wxID_HIGHEST + 21;
    constexpr int ID_OPEN_IN_NEW_TILE        = wxID_HIGHEST + 22;
    constexpr int ID_OPEN_IN_NEW_WINDOW_TERM = wxID_HIGHEST + 23;
    constexpr int ID_RESET_TERMINAL          = wxID_HIGHEST + 24;
    constexpr int ID_RESET_AND_CLEAR         = wxID_HIGHEST + 25;
    constexpr int ID_SEND_FILES              = wxID_HIGHEST + 26;
    constexpr int ID_RECEIVE_FILES           = wxID_HIGHEST + 27;
    constexpr int ID_RESTORE_SESSIONS        = wxID_HIGHEST + 28;
    constexpr int ID_SAVE_AS_SNAPSHOT        = wxID_HIGHEST + 29;
    constexpr int ID_OPEN_SNAPSHOT           = wxID_HIGHEST + 30;
    constexpr int ID_ABOUT                   = wxID_HIGHEST + 31;

    static bool IsValidSnapshotName(const std::string& s)
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

    // ---- Edit menu (populated by UIManager) ----------------------------------
    m_editMenu = new wxMenu;

    // ---- Connection menu -----------------------------------------------------
    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_NEW_CONNECTION,         "New Connection\tCtrl+Shift+N");
    m_connMenu->Append(ID_NEW_CONNECTION_IN_TILE, "New Connection in Tab\tCtrl+Shift+T");
    m_connMenu->Append(ID_CONNECTION_MANAGER,     "Connection Manager...\tCtrl+Shift+M");
    m_connMenu->Append(ID_RESTORE_SESSIONS,       "Restore Previous Session(s)");
    m_connMenu->Append(ID_SAVE_AS_SNAPSHOT,       "Save Session As...");
    m_connMenu->Append(ID_OPEN_SNAPSHOT,          "Open Saved Snapshot...");
    m_connMenu->AppendSeparator();
    m_connMenu->Append(ID_CLOSE_ACTIVE_SESSION,   "Close Active Session");
    m_connMenu->Append(wxID_CLOSE,                "Close This Window\tCtrl+Shift+Q");
    m_connMenu->Append(ID_QUIT_ALL,               "Close All\tCtrl+Shift+X");

    Bind(wxEVT_MENU, &MainFrame::OnNewConnection,             this, ID_NEW_CONNECTION);
    Bind(wxEVT_MENU, &MainFrame::OnNewConnectionInActiveTile, this, ID_NEW_CONNECTION_IN_TILE);
    Bind(wxEVT_MENU, &MainFrame::OnConnectionManager,         this, ID_CONNECTION_MANAGER);
    Bind(wxEVT_MENU, &MainFrame::OnRestoreSessions,           this, ID_RESTORE_SESSIONS);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(static_cast<App*>(wxTheApp)->HasRestoreSnapshot());
    }, ID_RESTORE_SESSIONS);
    Bind(wxEVT_MENU, &MainFrame::OnSaveAsSnapshot,            this, ID_SAVE_AS_SNAPSHOT);
    Bind(wxEVT_MENU, &MainFrame::OnOpenSnapshot,              this, ID_OPEN_SNAPSHOT);
    Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(static_cast<App*>(wxTheApp)->HasNamedSnapshots());
    }, ID_OPEN_SNAPSHOT);
    Bind(wxEVT_MENU, &MainFrame::OnCloseActiveSession,        this, ID_CLOSE_ACTIVE_SESSION);
    Bind(wxEVT_MENU, &MainFrame::OnCloseThisWindow,           this, wxID_CLOSE);
    Bind(wxEVT_MENU, &MainFrame::OnQuitAll,                   this, ID_QUIT_ALL);

    // ---- Terminal menu -------------------------------------------------------
    auto* termMenu = new wxMenu;
    m_miwrapMode  = termMenu->AppendCheckItem(ID_TOGGLE_wrapMode, "Toggle Wrap Mode\tCtrl+Shift+W");
    m_miBroadcast = termMenu->AppendCheckItem(ID_BROADCAST_MODE,  "Broadcast Input Mode\tCtrl+Shift+B");
    Bind(wxEVT_MENU, &MainFrame::OnTogglewrapMode,  this, ID_TOGGLE_wrapMode);
    Bind(wxEVT_MENU, &MainFrame::OnToggleBroadcast, this, ID_BROADCAST_MODE);

    termMenu->AppendSeparator();
    auto* geoMenu = new wxMenu;
    geoMenu->Append(ID_SET_GEOMETRY_80x24,  "80 x 24");
    geoMenu->Append(ID_SET_GEOMETRY_132x24, "132 x 24");
    geoMenu->Append(ID_SET_GEOMETRY_CUSTOM, "Custom...");
    termMenu->AppendSubMenu(geoMenu, "Set Geometry");
    termMenu->Append(ID_SET_FONT, "Set Font...");
    Bind(wxEVT_MENU, &MainFrame::OnSetGeometry80x24,  this, ID_SET_GEOMETRY_80x24);
    Bind(wxEVT_MENU, &MainFrame::OnSetGeometry132x24, this, ID_SET_GEOMETRY_132x24);
    Bind(wxEVT_MENU, &MainFrame::OnSetGeometryCustom, this, ID_SET_GEOMETRY_CUSTOM);
    Bind(wxEVT_MENU, &MainFrame::OnSetFont,           this, ID_SET_FONT);

    termMenu->AppendSeparator();
    termMenu->Append(ID_RESET_TERMINAL,  "Reset Terminal");
    termMenu->Append(ID_RESET_AND_CLEAR, "Reset and Clear...");
    Bind(wxEVT_MENU, &MainFrame::OnResetTerminal,  this, ID_RESET_TERMINAL);
    Bind(wxEVT_MENU, &MainFrame::OnResetAndClear,  this, ID_RESET_AND_CLEAR);

    termMenu->AppendSeparator();
    termMenu->Append(ID_SAVE_SESSION_FILE_TERM, "Save Session to File...");
    termMenu->Append(ID_SEND_FILES,              "Send Files to Remote...");
    termMenu->Append(ID_RECEIVE_FILES,           "Receive Files from Remote...");
    Bind(wxEVT_MENU, &MainFrame::OnSaveSessionFileTerminal, this, ID_SAVE_SESSION_FILE_TERM);
    Bind(wxEVT_MENU, &MainFrame::OnSendFiles,               this, ID_SEND_FILES);
    Bind(wxEVT_MENU, &MainFrame::OnReceiveFiles,            this, ID_RECEIVE_FILES);

    termMenu->AppendSeparator();
    termMenu->Append(ID_OPEN_IN_NEW_TILE,        "Move to New Tile");
    termMenu->Append(ID_OPEN_IN_NEW_WINDOW_TERM, "Move to New Window");
    Bind(wxEVT_MENU, &MainFrame::OnOpenInNewTile,        this, ID_OPEN_IN_NEW_TILE);
    Bind(wxEVT_MENU, &MainFrame::OnOpenInNewWindowTerminal, this, ID_OPEN_IN_NEW_WINDOW_TERM);

    // ---- Window menu (New Window static; rest populated dynamically) ---------
    m_windowMenu = new wxMenu;
    m_windowMenu->Append(ID_NEW_WINDOW, "New Window");
    m_windowMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnNewWindow,            this, ID_NEW_WINDOW);
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

    SetBackgroundColour(wxColour(40, 40, 40));
    SetSizer(new wxBoxSizer(wxVERTICAL));
    SetClientSize(wxSize(1200, 700));
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    if (m_uiManager) {
        m_uiManager->FireBeforeClose();
        m_uiManager->CloseAllSessions();
    }
    event.Skip();
}

void MainFrame::OnCloseThisWindow(wxCommandEvent&)
{
    Close(true);
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
    const std::string defaultShell = [] {
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/bash");
    }();

    ui::NewConnectionDialog dlg(this, defaultShell,
                                m_cfg.geometryPresets,
                                m_store.GetAll(),
                                context);
    if (dlg.ShowModal() != wxID_OK)
        return false;

    term::session::Connection conn = ui::ToConnection(dlg.GetParams(), ++m_sessionCount);

    // Honour "Save as Profile" if requested
    const std::string profileName = dlg.GetProfileName();
    if (dlg.GetSaveAsProfile() && !profileName.empty()) {
        conn.label = profileName;
        const std::string existingId = dlg.GetSelectedProfileId();
        if (!existingId.empty()) {
            const auto& profiles = m_store.GetAll();
            auto it = std::find_if(profiles.begin(), profiles.end(),
                                   [&](const term::db::ConnectionProfile& p){
                                       return p.id == existingId; });
            if (it != profiles.end()) {
                term::db::ConnectionProfile upd = *it;
                upd.name        = profileName;
                upd.transport   = conn.transport;
                upd.wrapMode    = conn.wrapMode;
                upd.columnWidth = conn.columnWidth;
                upd.rows        = conn.rows;
                m_store.Update(upd);
            }
        } else {
            m_store.Add(profileName, conn.transport,
                        conn.wrapMode, conn.columnWidth, conn.rows);
        }
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

void MainFrame::OnRestoreSessions(wxCommandEvent&)
{
    static_cast<App*>(wxTheApp)->RestoreSessionsFromMenu(this);
}

void MainFrame::OnSaveAsSnapshot(wxCommandEvent&)
{
    wxTextEntryDialog dlg(this, "Snapshot name:", "Save Session As");
    if (dlg.ShowModal() != wxID_OK) return;

    const std::string name = dlg.GetValue().Strip(wxString::both).ToStdString();
    if (!IsValidSnapshotName(name)) {
        wxMessageBox(
            "Name must be 1–64 characters: letters, digits, spaces, hyphens, underscores.",
            "Invalid Name", wxOK | wxICON_WARNING, this);
        return;
    }

    auto* app = static_cast<App*>(wxTheApp);
    if (app->HasNamedSnapshot(name)) {
        if (wxMessageBox("Overwrite existing snapshot '" + name + "'?",
                         "Confirm", wxYES_NO | wxICON_QUESTION, this) != wxYES)
            return;
    }
    app->SaveNamedSnapshot(name);
}

void MainFrame::OnOpenSnapshot(wxCommandEvent&)
{
    auto* app = static_cast<App*>(wxTheApp);
    const auto names = app->GetNamedSnapshotNames();
    if (names.empty()) return;

    ui::SnapshotManagerDialog dlg(this, names);
    const int result = dlg.ShowModal();

    for (const auto& n : dlg.GetDeletedNames())
        app->DeleteNamedSnapshot(n);

    if (result == wxID_OK)
        app->RestoreNamedSnapshot(dlg.GetSelectedName(), this);
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

    // Remove all items after the static "New Window" + separator (positions 0 and 1).
    while (m_windowMenu->GetMenuItemCount() > 2)
        m_windowMenu->Delete(m_windowMenu->FindItemByPosition(2));

    m_windowFrames.clear();
    m_windowSessions.clear();

    int windowId  = kWindowMenuBase;
    int sessionId = kWindowSessionBase;

    for (const auto& entry : entries) {
        auto* item = m_windowMenu->AppendCheckItem(windowId,
                         entry.title.empty() ? "naTE" : entry.title);
        if (entry.frame == this)
            item->Check(true);
        Bind(wxEVT_MENU, &MainFrame::OnWindowMenuItem, this, windowId);
        m_windowFrames.push_back(entry.frame);
        ++windowId;

        for (const auto& sess : entry.sessions) {
            m_windowMenu->Append(sessionId, "  " + sess.label);
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

void MainFrame::NotYetImplemented()
{
    wxMessageBox("Not yet implemented", "naTE", wxOK | wxICON_INFORMATION);
}

void MainFrame::OnCloseActiveSession(wxCommandEvent&)    { NotYetImplemented(); }
void MainFrame::OnSetGeometry80x24(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->SetGeometryForActive(80, 24);
}

void MainFrame::OnSetGeometry132x24(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->SetGeometryForActive(132, 24);
}

void MainFrame::OnSetGeometryCustom(wxCommandEvent&)
{
    if (!m_uiManager) return;
    const auto current  = m_uiManager->GetActiveGeometry();
    const auto defCols  = current ? current->cols : static_cast<unsigned short>(80);
    const auto defRows  = current ? current->rows : static_cast<unsigned short>(24);
    GeometryDialog dlg(this, defCols, defRows);
    if (dlg.ShowModal() == wxID_OK)
        m_uiManager->SetGeometryForActive(dlg.GetCols(), dlg.GetRows());
}
void MainFrame::OnSetFont(wxCommandEvent&)               { NotYetImplemented(); }
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

void MainFrame::OnSendFiles(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->SendFilesForActive();
}

void MainFrame::OnReceiveFiles(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->ReceiveFilesForActive();
}

void MainFrame::OnOpenInNewTile(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->MoveActiveSessionToNewTile();
}

void MainFrame::OnOpenInNewWindowTerminal(wxCommandEvent&)
{
    if (m_uiManager) m_uiManager->MoveActiveSessionToNewWindow();
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
