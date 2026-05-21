#pragma once
#include <wx/frame.h>
#include <wx/menu.h>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/Connection.h"
#include "session/ISessionObserver.h"
#include "ui/ISessionDropTarget.h"
#include "ui/NewConnectionDialog.h"

namespace term::db { class ConnectionStore; }
namespace ui { class UIManager; }
class TerminalTile;

class MainFrame : public wxFrame, public ui::ISessionDropTarget {
public:
    struct SessionMenuEntry {
        term::session::SessionId id;
        std::string              label;
    };
    struct WindowMenuEntry {
        MainFrame*                   frame;
        std::string                  title;
        std::vector<SessionMenuEntry> sessions;
    };

    MainFrame(const AppConfig& cfg,
              term::input::InputRouter& router,
              term::db::ConnectionStore& store);

    wxMenu* GetEditMenu() const { return m_editMenu; }

    // Called by App after UIManager construction to wire session/wrap mode calls.
    // Also appends the Preferences item to the Edit menu (which UIManager
    // has already populated by this point).
    void SetUIManager(ui::UIManager* ui);

    // Called by App::ApplyPreferences to keep the frame's config copy current.
    void UpdateConfig(const AppConfig& cfg) { m_cfg = cfg; }

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

    // Rebuilds the Window menu to reflect the current set of open frames and their sessions.
    void RebuildWindowMenu(const std::vector<WindowMenuEntry>& entries);

    // Activates a session owned by this window (called from Window menu lambdas).
    void ActivateSession(term::session::SessionId id);

    // ISessionDropTarget — no specific tile; App will create a new tile.
    bool DropSession(std::span<const term::session::SessionId> ids,
                     ui::DragIntent intent,
                     wxPoint screenPt) override;

private:
    void OnClose(wxCloseEvent& event);
    void OnCloseAllSessions(wxCommandEvent&);
    void OnCloseThisWindow(wxCommandEvent&);
    void OnQuitAll(wxCommandEvent&);
    void OnNewWindow(wxCommandEvent&);
    void OnNewConnection(wxCommandEvent&);
    void OnNewConnectionInActiveTile(wxCommandEvent&);
    void OnConnectionManager(wxCommandEvent&);
    void OnRestoreSessions(wxCommandEvent&);
    void OnSaveAsSnapshot(wxCommandEvent&);
    void OnOpenSnapshot(wxCommandEvent&);
    void OnTogglewrapMode(wxCommandEvent&);
    void OnToggleBroadcast(wxCommandEvent&);
    void OnWindowMenuItem(wxCommandEvent& evt);
    void OnWindowSessionMenuItem(wxCommandEvent& evt);

    void OnPreferences(wxCommandEvent&);

    // Stub handlers — display "Not yet implemented" until wired up.
    void NotYetImplemented();
    void OnSetGeometry80x24(wxCommandEvent&);
    void OnSetGeometry132x24(wxCommandEvent&);
    void OnSetGeometryCustom(wxCommandEvent&);
    void OnSetFont(wxCommandEvent&);
    void OnResetTerminal(wxCommandEvent&);
    void OnResetAndClear(wxCommandEvent&);
    void OnSaveSessionFileTerminal(wxCommandEvent&);
    void OnSendFiles(wxCommandEvent&);
    void OnReceiveFiles(wxCommandEvent&);
    void OnOpenInNewTile(wxCommandEvent&);
    void OnOpenInNewWindowTerminal(wxCommandEvent&);
    void OnRefitWindow(wxCommandEvent&);
    void OnAbout(wxCommandEvent&);

    // Shows the New Connection dialog with the given context, saves any profile
    // if requested, then launches the connection according to the chosen placement.
    // Returns false if the user cancelled.
    bool RunConnectionDialog(ui::LaunchContext context,
                             TerminalTile* targetTile = nullptr);

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

    // Parallel to window menu items; used by OnWindowMenuItem.
    std::vector<MainFrame*> m_windowFrames;

    // Parallel to session sub-items in Window menu; used by session lambdas.
    std::vector<std::pair<MainFrame*, term::session::SessionId>> m_windowSessions;
};
