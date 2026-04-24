#pragma once
#include <wx/frame.h>
#include <wx/menu.h>
#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/SessionManager.h"
#include "ui/NewConnectionDialog.h"

class MainFrame : public wxFrame {
public:
    MainFrame(const AppConfig& cfg,
              term::input::InputRouter& router,
              term::session::SessionManager& sm);

    wxMenu* GetConnMenu() const { return m_connMenu; }

private:
    void OnQuit(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);

    term::input::InputRouter&       m_router;
    term::session::SessionManager&  m_sm;
    AppConfig                       m_cfg;
    wxMenu*                         m_connMenu = nullptr;
    int                             m_sessionCount = 0;
};
