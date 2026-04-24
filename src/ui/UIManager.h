#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <wx/menu.h>

#include "config/Config.h"
#include "session/ISessionObserver.h"
#include "session/SessionManager.h"

class MainFrame;
class TerminalPanel;

namespace ui {

class UIManager : public term::session::ISessionObserver {
public:
    UIManager(term::session::SessionManager& sm,
              wxMenu*                        connMenu,
              MainFrame*                     frame,
              const AppConfig&               cfg);

    // ISessionObserver — all five callbacks documented on the interface
    void OnSessionCreated(term::session::SessionId, const std::string& label) override;
    void OnSessionTitleChanged(term::session::SessionId, const std::string& title) override;
    void OnSessionRefresh(term::session::SessionId) override;
    void OnSessionDisconnected(term::session::SessionId) override;
    void OnSessionDestroyed(term::session::SessionId) override;

    void RequestActivate(term::session::SessionId id);

    // Routed from TerminalPanel callbacks — always called on the UI thread
    void OnScroll(term::session::SessionId id, int topRow);
    void OnViewportResize(term::session::SessionId id, unsigned short cols, unsigned short rows);

private:
    static constexpr int kMenuIdBase = wxID_HIGHEST + 200;

    struct SessionUI {
        term::session::SessionId id       = 0;
        std::string              label;
        int                      menuId   = 0;
        TerminalPanel*           panel    = nullptr; // wx-parent-owned; non-owning here
    };

    SessionUI* FindSessionUI(term::session::SessionId id);

    term::session::SessionManager& sm_;
    wxMenu*                        connMenu_;
    MainFrame*                     frame_;
    AppConfig                      cfg_;

    std::unordered_map<term::session::SessionId, SessionUI> sessions_;
    int nextMenuId_ = kMenuIdBase;
};

} // namespace ui
