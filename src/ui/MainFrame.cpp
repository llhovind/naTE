#include "ui/MainFrame.h"
#include "ui/ConnectionManagerDialog.h"
#include "db/ConnectionStore.h"
#include <wx/sizer.h>
#include <cstdlib>

namespace {
    constexpr int ID_NEW_CONNECTION     = wxID_HIGHEST + 1;
    constexpr int ID_TOGGLE_WORDWRAP    = wxID_HIGHEST + 2;
    constexpr int ID_CONNECTION_MANAGER = wxID_HIGHEST + 3;

    template<class... Ts>
    struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts>
    overloaded(Ts...) -> overloaded<Ts...>;
}

MainFrame::MainFrame(const AppConfig& cfg,
                     term::input::InputRouter& router,
                     term::session::SessionManager& sm,
                     term::db::ConnectionStore& store)
    : wxFrame(nullptr, wxID_ANY, "naTE"),
      m_router(router),
      m_sm(sm),
      m_store(store),
      m_cfg(cfg)
{
    auto* fileMenu = new wxMenu;
    fileMenu->Append(wxID_EXIT, "Quit\tCtrl+Q");
    Bind(wxEVT_MENU,         &MainFrame::OnQuit,  this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    m_editMenu = new wxMenu;

    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_CONNECTION_MANAGER, "Connection Manager...\tCtrl+Shift+M");
    m_connMenu->Append(ID_NEW_CONNECTION,     "New Connection\tCtrl+N");
    m_connMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnConnectionManager, this, ID_CONNECTION_MANAGER);
    Bind(wxEVT_MENU, &MainFrame::OnNewConnection,     this, ID_NEW_CONNECTION);

    auto* termMenu = new wxMenu;
    m_miWordWrap = termMenu->AppendCheckItem(ID_TOGGLE_WORDWRAP, "Word Wrap\tCtrl+W");
    Bind(wxEVT_MENU, &MainFrame::OnToggleWordWrap, this, ID_TOGGLE_WORDWRAP);

    auto* menuBar = new wxMenuBar;
    menuBar->Append(fileMenu,   "&File");
    menuBar->Append(m_editMenu, "&Edit");
    menuBar->Append(m_connMenu, "&Connection");
    menuBar->Append(termMenu,   "&Terminal");
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

    SetBackgroundColour(wxColour(211, 211, 211));
    SetSizer(new wxBoxSizer(wxVERTICAL));
    SetClientSize(wxSize(810, 470));
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    m_sm.CloseAllSessions();
    event.Skip();
}

void MainFrame::OnQuit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::LaunchSession(const term::session::Connection& conn)
{
    const unsigned short cols = conn.columnWidth
        ? conn.columnWidth
        : static_cast<unsigned short>(m_cfg.columns);

    m_sm.CreateSession(conn,
                       m_cfg.scrollbackLines,
                       cols,
                       static_cast<unsigned short>(m_cfg.rows),
                       static_cast<unsigned short>(m_cfg.ptyLineWidth));
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

    LaunchSession(conn);
}

void MainFrame::OnConnectionManager(wxCommandEvent&)
{
    ui::ConnectionManagerDialog dlg(this, m_store, m_cfg,
        [this](const term::session::Connection& conn) {
            ++m_sessionCount;
            LaunchSession(conn);
        });
    dlg.ShowModal();
}

void MainFrame::OnToggleWordWrap(wxCommandEvent&)
{
    const term::session::SessionId id = m_sm.GetActiveSessionId();
    if (id == 0) return;
    const bool newWrap = !m_sm.GetDocLayout(id).GetWordWrap();
    m_sm.SetWordWrap(id, newWrap);
    m_miWordWrap->Check(newWrap);
}

void MainFrame::SyncWordWrapMenuItem(bool checked)
{
    if (m_miWordWrap)
        m_miWordWrap->Check(checked);
}
