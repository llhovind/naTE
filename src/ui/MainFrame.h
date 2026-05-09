#pragma once
#include <wx/frame.h>
#include <wx/menu.h>
#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/Connection.h"
#include "session/SessionManager.h"
#include "ui/NewConnectionDialog.h"

namespace term::db { class ConnectionStore; }

class MainFrame : public wxFrame {
public:
    MainFrame(const AppConfig& cfg,
              term::input::InputRouter& router,
              term::session::SessionManager& sm,
              term::db::ConnectionStore& store);

    wxMenu* GetConnMenu() const { return m_connMenu; }
    wxMenu* GetEditMenu() const { return m_editMenu; }

    // Called by UIManager::UpdateStatusBar to keep the menu check in sync.
    void SyncWordWrapMenuItem(bool checked);

private:
    void OnClose(wxCloseEvent& event);
    void OnQuit(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);
    void OnConnectionManager(wxCommandEvent&);
    void OnToggleWordWrap(wxCommandEvent&);

    void LaunchSession(const term::session::Connection& conn);

    term::input::InputRouter&       m_router;
    term::session::SessionManager&  m_sm;
    term::db::ConnectionStore&      m_store;
    AppConfig                       m_cfg;
    wxMenu*                         m_connMenu   = nullptr;
    wxMenu*                         m_editMenu   = nullptr;
    wxMenuItem*                     m_miWordWrap = nullptr;
    int                             m_sessionCount = 0;
};
