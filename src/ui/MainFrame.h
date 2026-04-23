#pragma once
#include <wx/frame.h>
#include <vector>
#include <memory>
#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/Session.h"
#include "ui/NewConnectionDialog.h"

class TerminalPanel;

class MainFrame : public wxFrame {
public:
    MainFrame(const AppConfig& cfg, term::input::InputRouter& router);

    TerminalPanel* GetPanel() const { return m_panel; }

private:
    struct ConnectionRecord {
        std::unique_ptr<term::session::Session> session;
        wxString label;
        int      menuId;
    };

    void OnQuit(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);

    void CreateConnection();
    void ActivateSession(ConnectionRecord& rec);

    term::input::InputRouter&        m_router;
    AppConfig                        m_cfg;
    std::vector<ConnectionRecord>    m_sessions;
    ConnectionRecord*                m_active = nullptr;
    wxMenu*                          m_connMenu = nullptr;

    TerminalPanel* m_panel = nullptr;
};
