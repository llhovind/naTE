#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include "transport/LoopbackTransport.h"
#include "transport/PtyTransport.h"
#include <wx/menu.h>
#include <wx/sizer.h>
#include <algorithm>
#include <cstdlib>

namespace {
    constexpr int ID_NEW_CONNECTION   = wxID_HIGHEST + 1;
    constexpr int ID_SESSION_BASE     = wxID_HIGHEST + 100; // per-session menu items start here
}

MainFrame::MainFrame(const AppConfig& cfg, term::input::InputRouter& router)
    : wxFrame(nullptr, wxID_ANY, "naTE"),
      m_router(router),
      m_cfg(cfg)
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

    SetBackgroundColour(wxColour(211, 211, 211));
    SetSizer(new wxBoxSizer(wxVERTICAL));
    SetClientSize(wxSize(810, 470));
}

void MainFrame::OnQuit(wxCommandEvent&)
{
    Close(true);
}

void MainFrame::OnNewConnection(wxCommandEvent&)
{
    CreateConnection();
}

namespace
{
    // Overload helper for std::visit
    template<class... Ts>
    struct overloaded : Ts... { using Ts::operator()...; };
    template<class... Ts>
    overloaded(Ts...) -> overloaded<Ts...>;
}

void MainFrame::CreateConnection()
{
    const std::string defaultShell = [] {
        const char* s = std::getenv("SHELL");
        return s ? std::string(s) : std::string("/bin/bash");
    }();

    ui::NewConnectionDialog dlg(this, defaultShell);
    if (dlg.ShowModal() != wxID_OK)
        return;

    std::unique_ptr<term::transport::Transport> transport;
    wxString label;
    const int idx = (int)m_sessions.size() + 1;

    std::visit(overloaded{
        [&](const ui::LoopbackParams&) {
            transport = std::make_unique<term::transport::LoopbackTransport>();
            label = wxString::Format("Loopback %d", idx);
        },
        [&](const ui::PtyParams& p) {
            transport = std::make_unique<term::transport::PtyTransport>(
                p.shell,
                static_cast<unsigned short>(m_cfg.columns),
                static_cast<unsigned short>(m_cfg.rows));
            label = wxString::Format("Local Shell %d", idx);
        }
    }, dlg.GetParams());

    if (!m_panel) {
        m_panel = new TerminalPanel(this, m_cfg);
        GetSizer()->Add(m_panel, 1, wxEXPAND);
        Layout();
        Fit();
    }

    const int id = ID_SESSION_BASE + idx;

    m_sessions.emplace_back();
    ConnectionRecord& rec = m_sessions.back();
    rec.session = std::make_unique<term::session::Session>(std::move(transport),
                                                           m_cfg.scrollbackLines);
    rec.label  = label;
    rec.menuId = id;

    rec.session->SetTitleCallback([this, &rec](const std::string& title) {
        rec.label = wxString::FromUTF8(title);
        m_connMenu->SetLabel(rec.menuId, rec.label);
        if (m_active == &rec)
            SetStatusText(wxString::Format("Active: %s", rec.label));
    });

    rec.session->SetDisconnectCallback([this, &rec]() {
        CallAfter([this, &rec]() { CloseConnection(rec); });
    });

    m_connMenu->Append(id, label);
    Bind(wxEVT_MENU, [this, &rec](wxCommandEvent&) { ActivateSession(rec); }, id);

    ActivateSession(rec);
}

void MainFrame::CloseConnection(ConnectionRecord& rec)
{
    if (m_active == &rec) {
        m_router.RemoveTarget(rec.session.get());
        m_active = nullptr;
        if (m_panel)
            m_panel->SetDocLayout(nullptr);
    }

    m_connMenu->Delete(rec.menuId);

    m_sessions.erase(
        std::remove_if(m_sessions.begin(), m_sessions.end(),
            [&rec](const ConnectionRecord& r) { return &r == &rec; }),
        m_sessions.end());

    if (m_sessions.empty()) {
        SetStatusText("No active connections");
        if (m_panel) {
            m_panel->Destroy();
            m_panel = nullptr;
        }
        Layout();
    } else if (!m_active) {
        ActivateSession(m_sessions.back());
    }
}

void MainFrame::ActivateSession(ConnectionRecord& rec)
{
    if (m_active)
        m_router.RemoveTarget(m_active->session.get());

    m_active = &rec;
    m_panel->SetDocLayout(&rec.session->GetDocLayout());
    rec.session->SetRefreshCallback([this] { m_panel->OnDocumentUpdate(); });
    m_router.AddTarget(rec.session.get());
    m_router.SetFocused(rec.session.get());
    SetStatusText(wxString::Format("Active: %s", rec.label));
}
