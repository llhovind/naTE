#pragma once
#include <wx/frame.h>
#include <wx/menu.h>
#include <string>
#include <utility>
#include <vector>
#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/Connection.h"
#include "ui/NewConnectionDialog.h"

namespace term::db { class ConnectionStore; }
namespace ui { class UIManager; }
class TerminalTile;

class MainFrame : public wxFrame {
public:
    MainFrame(const AppConfig& cfg,
              term::input::InputRouter& router,
              term::db::ConnectionStore& store);

    wxMenu* GetConnMenu() const { return m_connMenu; }
    wxMenu* GetEditMenu() const { return m_editMenu; }

    // Called by App after UIManager construction to wire session/wrap mode calls.
    void SetUIManager(ui::UIManager* ui) { m_uiManager = ui; }

    // Called by UIManager to keep the wrap mode menu check in sync.
    void SyncwrapModeMenuItem(bool checked);

    // Called by UIManager to keep the broadcast mode menu check in sync.
    void SyncBroadcastMenuItem(bool checked);

    // Opens a connection in this window in a new tile.
    void LaunchSession(const term::session::Connection& conn);

    // Opens a connection in this window as a new tab inside targetTile.
    // If targetTile is nullptr, falls back to LaunchSession (new tile).
    void LaunchSessionInTile(const term::session::Connection& conn,
                             TerminalTile* targetTile);

    // Called by UIManager when the user presses "+" in a tile's tab strip.
    // Opens the New Connection dialog and routes the result to targetTile.
    void LaunchNewConnectionInTile(TerminalTile* targetTile);

    // Rebuilds the Window menu to reflect the current set of open frames.
    void RebuildWindowMenu(const std::vector<std::pair<MainFrame*, std::string>>& entries);

private:
    void OnClose(wxCloseEvent& event);
    void OnCloseThisWindow(wxCommandEvent&);
    void OnQuitAll(wxCommandEvent&);
    void OnNewWindow(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);
    void OnNewConnectionInActiveTile(wxCommandEvent&);
    void OnConnectionManager(wxCommandEvent&);
    void OnTogglewrapMode(wxCommandEvent&);
    void OnToggleBroadcast(wxCommandEvent&);
    void OnWindowMenuItem(wxCommandEvent& evt);

    // Shared dialog flow: shows the New Connection dialog and returns a filled
    // Connection, or returns false if the user cancelled.
    bool RunNewConnectionDialog(term::session::Connection& conn,
                                bool& openInNewWindow);

    term::input::InputRouter&    m_router;
    term::db::ConnectionStore&   m_store;
    AppConfig                    m_cfg;
    ui::UIManager*               m_uiManager  = nullptr;
    wxMenu*                      m_connMenu   = nullptr;
    wxMenu*                      m_editMenu   = nullptr;
    wxMenu*                      m_windowMenu  = nullptr;
    wxMenuItem*                  m_miwrapMode  = nullptr;
    wxMenuItem*                  m_miBroadcast = nullptr;
    int                          m_sessionCount = 0;

    // Parallel to the current window menu items; used by OnWindowMenuItem.
    std::vector<MainFrame*> m_windowFrames;
};
