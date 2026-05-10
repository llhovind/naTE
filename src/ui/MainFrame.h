#pragma once
#include <wx/frame.h>
#include <wx/menu.h>
#include <string>
#include <utility>
#include <vector>
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

    // Called by UIManager to keep the word-wrap menu check in sync.
    void SyncWordWrapMenuItem(bool checked);

    // Opens a connection in this window's TerminalGrid.
    void LaunchSession(const term::session::Connection& conn);

    // Rebuilds the Window menu to reflect the current set of open frames.
    // entries: {frame*, title} for each open window, ordered by creation time.
    void RebuildWindowMenu(const std::vector<std::pair<MainFrame*, std::string>>& entries);

private:
    void OnClose(wxCloseEvent& event);
    void OnCloseThisWindow(wxCommandEvent&);
    void OnQuitAll(wxCommandEvent&);
    void OnNewWindow(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);
    void OnConnectionManager(wxCommandEvent&);
    void OnToggleWordWrap(wxCommandEvent&);
    void OnWindowMenuItem(wxCommandEvent& evt);

    term::input::InputRouter&       m_router;
    term::session::SessionManager&  m_sm;
    term::db::ConnectionStore&      m_store;
    AppConfig                       m_cfg;
    wxMenu*                         m_connMenu   = nullptr;
    wxMenu*                         m_editMenu   = nullptr;
    wxMenu*                         m_windowMenu = nullptr;
    wxMenuItem*                     m_miWordWrap = nullptr;
    int                             m_sessionCount = 0;

    // Parallel to the current window menu items; used by OnWindowMenuItem.
    std::vector<MainFrame*> m_windowFrames;
};
