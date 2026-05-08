#include "ui/MainFrame.h"
#include <wx/sizer.h>
#include <cstdlib>

namespace {
    constexpr int ID_NEW_CONNECTION  = wxID_HIGHEST + 1;
    constexpr int ID_TOGGLE_WORDWRAP = wxID_HIGHEST + 2;

    template<class... Ts>
    struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts>
    overloaded(Ts...) -> overloaded<Ts...>;
}

MainFrame::MainFrame(const AppConfig& cfg,
                     term::input::InputRouter& router,
                     term::session::SessionManager& sm)
    : wxFrame(nullptr, wxID_ANY, "naTE"),
      m_router(router),
      m_sm(sm),
      m_cfg(cfg)
{
    auto* fileMenu = new wxMenu;
    fileMenu->Append(wxID_EXIT, "Quit\tCtrl+Q");
    Bind(wxEVT_MENU,         &MainFrame::OnQuit,  this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    m_editMenu = new wxMenu;

    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_NEW_CONNECTION, "New Connection\tCtrl+N");
    m_connMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnNewConnection, this, ID_NEW_CONNECTION);

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
    SetStatusText("Ready — use Connection > New Connection to start", 1);
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

void MainFrame::OnNewConnection(wxCommandEvent&)
{
    const std::string defaultShell = [] {
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/bash");
    }();

    ui::NewConnectionDialog dlg(this, defaultShell);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const int idx = ++m_sessionCount;
    term::session::Connection conn;
    bool wordWrap = false;
    std::visit(overloaded{
        [&](const ui::LoopbackParams&) {
            conn = { wxString::Format("Loopback %d", idx).ToStdString(),
                     term::session::LoopbackDesc{} };
        },
        [&](const ui::PtyParams& p) {
            conn = { wxString::Format("Local Shell %d", idx).ToStdString(),
                     term::session::PtyDesc{ p.shell } };
            wordWrap = p.wordWrap;
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
            conn = { wxString::Format("SSH %s@%s:%d #%d",
                                      p.username, p.host,
                                      static_cast<int>(p.port), idx).ToStdString(),
                     d };
        }
    }, dlg.GetParams());

    m_sm.CreateSession(conn,
                       m_cfg.scrollbackLines,
                       static_cast<unsigned short>(m_cfg.columns),
                       static_cast<unsigned short>(m_cfg.rows),
                       static_cast<unsigned short>(m_cfg.ptyLineWidth),
                       wordWrap);
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
