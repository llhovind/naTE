#include "ui/MainFrame.h"
#include "ui/ConnectionManagerDialog.h"
#include "ui/UIManager.h"
#include "db/ConnectionStore.h"
#include "app/App.h"
#include <wx/sizer.h>
#include <cstdlib>

namespace {
    constexpr int ID_NEW_CONNECTION     = wxID_HIGHEST + 1;
    constexpr int ID_TOGGLE_WORDWRAP    = wxID_HIGHEST + 2;
    constexpr int ID_CONNECTION_MANAGER = wxID_HIGHEST + 3;
    constexpr int ID_NEW_WINDOW         = wxID_HIGHEST + 4;
    constexpr int ID_QUIT_ALL           = wxID_HIGHEST + 5;
    constexpr int ID_BROADCAST_MODE     = wxID_HIGHEST + 6;

    // Window menu items occupy [kWindowMenuBase, kWindowMenuBase + kWindowMenuMax).
    constexpr int kWindowMenuBase = wxID_HIGHEST + 400;
    constexpr int kWindowMenuMax  = 64;

    template<class... Ts>
    struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts>
    overloaded(Ts...) -> overloaded<Ts...>;
}

MainFrame::MainFrame(const AppConfig& cfg,
                     term::input::InputRouter& router,
                     term::db::ConnectionStore& store)
    : wxFrame(nullptr, wxID_ANY, "naTE"),
      m_router(router),
      m_store(store),
      m_cfg(cfg)
{
    // ---- File menu -----------------------------------------------------------
    auto* fileMenu = new wxMenu;
    fileMenu->Append(ID_NEW_WINDOW, "New Window\tCtrl+Shift+N");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_CLOSE,  "Close This Window\tCtrl+Shift+Q");
    fileMenu->Append(ID_QUIT_ALL, "Quit All\tCtrl+Shift+X");

    Bind(wxEVT_MENU,         &MainFrame::OnNewWindow,        this, ID_NEW_WINDOW);
    Bind(wxEVT_MENU,         &MainFrame::OnCloseThisWindow,  this, wxID_CLOSE);
    Bind(wxEVT_MENU,         &MainFrame::OnQuitAll,          this, ID_QUIT_ALL);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose,            this);

    // ---- Edit menu (populated by UIManager) ----------------------------------
    m_editMenu = new wxMenu;

    // ---- Connection menu -----------------------------------------------------
    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_CONNECTION_MANAGER, "Connection Manager...\tCtrl+Shift+M");
    m_connMenu->Append(ID_NEW_CONNECTION,     "New Connection");
    m_connMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnConnectionManager, this, ID_CONNECTION_MANAGER);
    Bind(wxEVT_MENU, &MainFrame::OnNewConnection,     this, ID_NEW_CONNECTION);

    // ---- Terminal menu -------------------------------------------------------
    auto* termMenu = new wxMenu;
    m_miWordWrap  = termMenu->AppendCheckItem(ID_TOGGLE_WORDWRAP,  "Word Wrap\tCtrl+Shift+W");
    m_miBroadcast = termMenu->AppendCheckItem(ID_BROADCAST_MODE,   "Broadcast Input Mode\tCtrl+Shift+B");
    Bind(wxEVT_MENU, &MainFrame::OnToggleWordWrap, this, ID_TOGGLE_WORDWRAP);
    Bind(wxEVT_MENU, &MainFrame::OnToggleBroadcast, this, ID_BROADCAST_MODE);

    // ---- Window menu (populated dynamically) ---------------------------------
    m_windowMenu = new wxMenu;

    auto* menuBar = new wxMenuBar;
    menuBar->Append(fileMenu,      "&File");
    menuBar->Append(m_editMenu,    "&Edit");
    menuBar->Append(m_connMenu,    "&Connection");
    menuBar->Append(termMenu,      "&Terminal");
    menuBar->Append(m_windowMenu,  "&Window");
    SetMenuBar(menuBar);

    CreateStatusBar(4);
    {
        static constexpr int kWidths[] = { 80, -1, 90, 130 };
        SetStatusWidths(4, kWidths);
    }
    SetStatusText("",     0);
    SetStatusText("Ready — use Connection > Connection Manager to start", 1);
    SetStatusText("",     2);
    SetStatusText("",     3);

    SetBackgroundColour(wxColour(40, 40, 40));
    SetSizer(new wxBoxSizer(wxVERTICAL));
    SetClientSize(wxSize(1200, 700));
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    if (m_uiManager)
        m_uiManager->CloseAllSessions();
    event.Skip();
}

void MainFrame::OnCloseThisWindow(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnQuitAll(wxCommandEvent&)
{
    static_cast<App*>(wxTheApp)->QuitAll();
}

void MainFrame::OnNewWindow(wxCommandEvent&)
{
    static_cast<App*>(wxTheApp)->CreateNewWindow();
}

void MainFrame::LaunchSession(const term::session::Connection& conn)
{
    static_cast<App*>(wxTheApp)->CreateSessionInWindow(conn, this);
}

void MainFrame::OnNewConnection(wxCommandEvent&)
{
    const std::string defaultShell = [] {
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/bash");
    }();

    ui::NewConnectionDialog dlg(this, defaultShell, m_cfg.columnWidths);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const int idx = ++m_sessionCount;
    term::session::Connection conn;
    std::visit(overloaded{
        [&](const ui::LoopbackParams& p) {
            conn.label       = wxString::Format("Loopback %d", idx).ToStdString();
            conn.transport   = term::session::LoopbackDesc{};
            conn.wordWrap    = p.wordWrap;
            conn.columnWidth = p.columnWidth;
        },
        [&](const ui::PtyParams& p) {
            conn.label       = wxString::Format("Local Shell %d", idx).ToStdString();
            conn.transport   = term::session::PtyDesc{ p.shell };
            conn.wordWrap    = p.wordWrap;
            conn.columnWidth = p.columnWidth;
        },
        [&](const ui::SshParams& p) {
            term::session::SshDesc d;
            d.host              = p.host;
            d.port              = p.port;
            d.username          = p.username;
            d.connectTimeoutSec = p.connectTimeoutSec;
            d.keepaliveSeconds  = p.keepaliveSeconds;
            d.remoteCommand     = p.remoteCommand;
            d.compress          = p.compress;
            d.password          = p.password;
            d.privateKeyPath    = p.privateKeyPath;
            d.passphrase        = p.passphrase;
            switch (p.authMethod) {
                case ui::SshAuthChoice::Agent:
                    d.authMethod = term::session::SshAuthMethod::Agent; break;
                case ui::SshAuthChoice::Password:
                    d.authMethod = term::session::SshAuthMethod::Password; break;
                case ui::SshAuthChoice::PrivateKey:
                    d.authMethod = term::session::SshAuthMethod::PrivateKey; break;
            }
            conn.label       = wxString::Format("SSH %s@%s:%d #%d",
                                                p.username, p.host,
                                                static_cast<int>(p.port), idx).ToStdString();
            conn.transport   = d;
            conn.wordWrap    = p.wordWrap;
            conn.columnWidth = p.columnWidth;
        }
    }, dlg.GetParams());

    // If the user provided a name, override the auto-generated label and
    // persist the profile to the Connection Manager for future use.
    const std::string name = dlg.GetConnectionName();
    if (!name.empty()) {
        conn.label = name;
        m_store.Add(name, conn.transport, conn.wordWrap, conn.columnWidth);
    }

    if (dlg.GetOpenInNewWindow()) {
        auto* newFrame = static_cast<App*>(wxTheApp)->CreateNewWindow();
        newFrame->LaunchSession(conn);
    } else {
        LaunchSession(conn);
    }
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

void MainFrame::OnToggleWordWrap(wxCommandEvent&)
{
    if (m_uiManager)
        m_uiManager->ToggleWordWrapForActive();
}

void MainFrame::SyncWordWrapMenuItem(bool checked)
{
    if (m_miWordWrap)
        m_miWordWrap->Check(checked);
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

void MainFrame::RebuildWindowMenu(const std::vector<std::pair<MainFrame*, std::string>>& entries)
{
    // Unbind handlers for any previously populated items.
    for (int i = 0; i < static_cast<int>(m_windowFrames.size()); ++i)
        Unbind(wxEVT_MENU, &MainFrame::OnWindowMenuItem, this, kWindowMenuBase + i);

    while (m_windowMenu->GetMenuItemCount() > 0)
        m_windowMenu->Delete(m_windowMenu->FindItemByPosition(0));

    m_windowFrames.clear();

    int id = kWindowMenuBase;
    for (const auto& [frame, title] : entries) {
        auto* item = m_windowMenu->AppendCheckItem(id, title.empty() ? "naTE" : title);
        if (frame == this)
            item->Check(true);
        Bind(wxEVT_MENU, &MainFrame::OnWindowMenuItem, this, id);
        m_windowFrames.push_back(frame);
        ++id;
    }
}

void MainFrame::OnWindowMenuItem(wxCommandEvent& evt)
{
    const int idx = evt.GetId() - kWindowMenuBase;
    if (idx >= 0 && idx < static_cast<int>(m_windowFrames.size()))
        m_windowFrames[idx]->Raise();
}
