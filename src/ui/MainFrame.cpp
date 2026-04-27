#include "ui/MainFrame.h"
#include <wx/sizer.h>
#include <cstdlib>

namespace {
    constexpr int ID_NEW_CONNECTION = wxID_HIGHEST + 1;

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

    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_NEW_CONNECTION, "New Connection\tCtrl+N");
    m_connMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnNewConnection, this, ID_NEW_CONNECTION);

    auto* menuBar = new wxMenuBar;
    menuBar->Append(fileMenu,   "&File");
    menuBar->Append(m_connMenu, "&Connection");
    SetMenuBar(menuBar);

    CreateStatusBar();
    SetStatusText("Ready — use Connection > New Connection to start");

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
    std::visit(overloaded{
        [&](const ui::LoopbackParams&) {
            conn = { wxString::Format("Loopback %d", idx).ToStdString(),
                     term::session::LoopbackDesc{} };
        },
        [&](const ui::PtyParams& p) {
            conn = { wxString::Format("Local Shell %d", idx).ToStdString(),
                     term::session::PtyDesc{ p.shell } };
        }
    }, dlg.GetParams());

    m_sm.CreateSession(conn,
                       m_cfg.scrollbackLines,
                       static_cast<unsigned short>(m_cfg.columns),
                       static_cast<unsigned short>(m_cfg.rows),
                       static_cast<unsigned short>(m_cfg.ptyLineWidth));
}
