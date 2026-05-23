#pragma once
#include <wx/app.h>
#include <wx/timer.h>
#include <memory>
#include <span>
#include <vector>
#include "config/Config.h"
#include "db/ConnectionStore.h"
#include "db/INamedSnapshotRepository.h"
#include "db/ISessionRestoreRepository.h"
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

    bool IsSessionManagerShutdown() const { return m_sessionManagerShutdown; }

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

    // Returns true if a valid session restore snapshot exists.
    bool HasRestoreSnapshot() const;

    // Saves the current session layout as a restore snapshot.
    // Safe to call from any context (timer, close hook, or menu).
    void SaveRestoreSnapshot();

    // Called from the "Restore Previous Session(s)" menu item.
    // callerFrame is the frame that triggered the action; it will be reused
    // as the first restored window if it currently has no sessions.
    void RestoreSessionsFromMenu(MainFrame* callerFrame);

    // Returns the absolute path to config.ini used at startup.
    const std::string& GetConfigPath() const { return m_configPath; }

    // Returns the user themes directory (~/.nate/themes).
    const std::string& GetThemesDir() const { return m_themesDir; }

    // Persists updated config to disk and replaces the in-memory copy.
    // New connections and tiles will use the updated settings.
    void ApplyPreferences(const AppConfig& cfg);

    // Named snapshot operations (Save Session As… / Open Saved Snapshot…).
    bool                     HasNamedSnapshots()                           const;
    std::vector<std::string> GetNamedSnapshotNames()                       const;
    bool                     HasNamedSnapshot(const std::string& name)     const;
    void                     SaveNamedSnapshot(const std::string& name);
    void                     RestoreNamedSnapshot(const std::string& name,
                                                  MainFrame* callerFrame);
    void                     DeleteNamedSnapshot(const std::string& name);

private:
    struct WindowContext {
        std::unique_ptr<term::input::InputRouter> router;
        std::unique_ptr<ui::UIManager>            uiManager;
        MainFrame*                                frame    = nullptr; // wx-owned
        int                                       windowId = 0;
    };

    static int  AcquireInstanceId();
    static void ReleaseInstanceId(int id);

    void OnQueryEndSession(wxCloseEvent& event);
    void OnEndSession(wxCloseEvent& event);
    void OnIdle(wxIdleEvent& event);

    WindowContext* FindContext(MainFrame* frame);
    WindowContext* FindContextForSession(term::session::SessionId id);

    void RestoreStateImpl(const term::session::RestoreState& state,
                          MainFrame* firstFrame);

    term::session::RestoreState BuildCurrentState() const;

    AppConfig                                           m_cfg;
    std::string                                         m_configPath;
    std::string                                         m_themesDir;
    std::unique_ptr<term::db::ConnectionStore>          m_connectionStore;
    std::unique_ptr<term::session::SessionManager>      m_sessionManager;
    std::unique_ptr<term::db::ISessionRestoreRepository> m_restoreRepo;
    std::unique_ptr<term::db::INamedSnapshotRepository>  m_namedRepo;
    wxTimer                                             m_saveTimer;
    std::vector<std::unique_ptr<WindowContext>>        m_windows;
    int                                                m_instanceId           = 0;
    int                                                m_nextWindowId         = 0;
    bool                                               m_sessionManagerShutdown = false;
};

wxDECLARE_APP(App);
