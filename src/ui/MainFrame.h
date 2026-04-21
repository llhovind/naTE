#pragma once
#include <wx/frame.h>
#include <vector>
#include <memory>
#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/Session.h"

class TerminalPanel;

class MainFrame : public wxFrame {
public:
    MainFrame(const AppConfig& cfg, term::input::InputRouter& router);

    TerminalPanel* GetPanel() const { return m_panel; }

private:
    void OnQuit(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);

    void CreateConnection();
    void ActivateSession(term::session::Session* s);

    term::input::InputRouter&                           m_router;
    std::vector<std::unique_ptr<term::session::Session>> m_sessions;
    term::session::Session*                             m_active = nullptr;
    wxMenu*                                             m_connMenu = nullptr;

    TerminalPanel* m_panel;
};
