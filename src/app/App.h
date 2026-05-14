#pragma once
#include <wx/app.h>
#include <memory>
#include <vector>
#include "config/Config.h"
#include "db/ConnectionStore.h"
#include "input/InputRouter.h"
#include "session/Connection.h"
#include "session/ISessionObserver.h"
#include "session/SessionManager.h"
#include "ui/UIManager.h"

class MainFrame;
class TerminalTile;

class App : public wxApp {
public:
    bool OnInit() override;
    int  OnExit() override;

    // Creates a new independent window; returns its frame.
    MainFrame* CreateNewWindow();

    // Creates a session in a new tile in the target window.
    term::session::SessionId CreateSessionInWindow(
        const term::session::Connection& conn, MainFrame* target);

    // Creates a session as a new tab in an existing tile.
    term::session::SessionId CreateSessionInTile(
        const term::session::Connection& conn,
        MainFrame*    target,
        TerminalTile* targetTile);

    // Moves a live session from one window's UIManager to another's.
    // dstTile may be nullptr (creates a new tile in the destination window).
    void MoveSession(MainFrame* src, term::session::SessionId id, MainFrame* dst,
                     TerminalTile* dstTile = nullptr);

    // Closes every open window and exits the application.
    void QuitAll();

    // Notifies all open windows to rebuild their Window menu.
    void RebuildWindowMenus();

private:
    struct WindowContext {
        std::unique_ptr<term::input::InputRouter> router;
        std::unique_ptr<ui::UIManager>            uiManager;
        MainFrame*                                frame = nullptr; // wx-owned
    };

    WindowContext* FindContext(MainFrame* frame);

    AppConfig                                      m_cfg;
    std::unique_ptr<term::db::ConnectionStore>     m_connectionStore;
    std::unique_ptr<term::session::SessionManager> m_sessionManager;
    std::vector<std::unique_ptr<WindowContext>>    m_windows;
};
