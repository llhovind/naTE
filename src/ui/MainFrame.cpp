#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include "transport/LoopbackTransport.h"
#include <wx/menu.h>
#include <wx/sizer.h>

namespace {
    constexpr int ID_NEW_CONNECTION   = wxID_HIGHEST + 1;
    constexpr int ID_SESSION_BASE     = wxID_HIGHEST + 100; // per-session menu items start here
}

MainFrame::MainFrame(const AppConfig& cfg, term::input::InputRouter& router)
    : wxFrame(nullptr, wxID_ANY, "naTE"),
      m_router(router)
{
    // File menu
    auto* fileMenu = new wxMenu;
    fileMenu->Append(wxID_EXIT, "Quit\tCtrl+Q");
    Bind(wxEVT_MENU, &MainFrame::OnQuit, this, wxID_EXIT);

    // Connection menu
    m_connMenu = new wxMenu;
    m_connMenu->Append(ID_NEW_CONNECTION, "New Connection\tCtrl+N");
    m_connMenu->AppendSeparator();
    Bind(wxEVT_MENU, &MainFrame::OnNewConnection, this, ID_NEW_CONNECTION);

    auto* menuBar = new wxMenuBar;
    menuBar->Append(fileMenu,    "&File");
    menuBar->Append(m_connMenu, "&Connection");
    SetMenuBar(menuBar);

    CreateStatusBar();
    SetStatusText("Ready — use Connection > New Connection to start");

    m_panel = new TerminalPanel(this, cfg);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_panel, 1, wxEXPAND);
    SetSizerAndFit(sizer);
}

void MainFrame::OnQuit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnNewConnection(wxCommandEvent&)
{
    CreateConnection();
}

void MainFrame::CreateConnection()
{
    auto transport = std::make_unique<term::transport::LoopbackTransport>();
    auto session   = std::make_unique<term::session::Session>(std::move(transport));
    term::session::Session* raw = session.get();
    m_sessions.push_back(std::move(session));

    const int    idx = (int)m_sessions.size();
    const int    id  = ID_SESSION_BASE + idx;
    const wxString label = wxString::Format("Connection %d", idx);
    m_connMenu->Append(id, label);
    Bind(wxEVT_MENU, [this, raw](wxCommandEvent&) { ActivateSession(raw); }, id);

    ActivateSession(raw);
    SetStatusText(wxString::Format("Active: Connection %d", idx));
}

void MainFrame::ActivateSession(term::session::Session* s)
{
    if (m_active)
        m_router.RemoveTarget(m_active);

    m_active = s;
    m_panel->SetLayout(&s->GetLayout());
    s->SetRefreshCallback([this] { m_panel->Refresh(); });
    m_router.AddTarget(s);
    m_router.SetFocused(s);
}
