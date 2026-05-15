#pragma once
#include <wx/app.h>
#include <memory>
#include <span>
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

    // Transfers sessions to a destination tile/frame.
    // dstFrame = nullptr → create a new MainFrame.
    // dstTile  = nullptr → create a new tile in dstFrame.
    // Returns true if accepted; source should call ReleaseSession on each id.
    bool DropSession(std::span<const term::session::SessionId> ids,
                     MainFrame*    dstFrame,
                     TerminalTile* dstTile);

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
    WindowContext* FindContextForSession(term::session::SessionId id);

    AppConfig                                      m_cfg;
    std::unique_ptr<term::db::ConnectionStore>     m_connectionStore;
    std::unique_ptr<term::session::SessionManager> m_sessionManager;
    std::vector<std::unique_ptr<WindowContext>>    m_windows;
};

wxDECLARE_APP(App);
