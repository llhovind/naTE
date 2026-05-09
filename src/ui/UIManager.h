#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <wx/menu.h>

#include "transport/TransportError.h"

#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/ISessionObserver.h"
#include "session/SessionManager.h"
#include "ui/SearchController.h"
#include "ui/SelectionActionRegistry.h"

class MainFrame;
class TerminalPanel;
class TerminalGrid;
class TerminalTile;

namespace ui {

class UIManager : public term::session::ISessionObserver {
public:
    UIManager(term::session::SessionManager& sm,
              wxMenu*                        connMenu,
              MainFrame*                     frame,
              const AppConfig&               cfg,
              term::input::InputRouter&      router,
              wxMenu*                        editMenu);

    // ISessionObserver — all six callbacks documented on the interface
    void OnSessionCreated(term::session::SessionId, const std::string& label, unsigned short cols) override;
    void OnSessionTitleChanged(term::session::SessionId, const std::string& title) override;
    void OnSessionRefresh(term::session::SessionId) override;
    void OnSessionError(term::session::SessionId,
                        const term::transport::TransportError& error) override;
    void OnSessionDisconnected(term::session::SessionId) override;
    void OnSessionDestroyed(term::session::SessionId) override;

    void RequestActivate(term::session::SessionId id);

    // Routed from TerminalPanel callbacks — always called on the UI thread
    void OnScroll(term::session::SessionId id, int topRow);
    void OnViewportResize(term::session::SessionId id, unsigned short cols, unsigned short rows);

    // Called on keyboard input to snap the active session's viewport to the cursor.
    void EnsureCursorVisibleForActive();

    // Search bar access — used by App::OnGdkKeyPress to route search keystrokes.
    SearchController* GetActiveSearchController();
    bool              SearchBarHasFocus() const;
    void              ShowSearchBarForActive(bool show,
                                             const std::u32string& initialQuery = {});
    // Returns the first line of the active session's selection (empty if none).
    std::u32string    GetActiveSelectedText() const;

private:
    static constexpr int kMenuIdBase      = wxID_HIGHEST + 200;

    // Edit menu item IDs
    static constexpr int kEditMenuCopy      = wxID_HIGHEST + 10;
    static constexpr int kEditMenuPaste     = wxID_HIGHEST + 11;
    static constexpr int kEditMenuSelectAll = wxID_HIGHEST + 12;
    static constexpr int kEditMenuPasteSel  = wxID_HIGHEST + 13;
    static constexpr int kEditMenuFind      = wxID_HIGHEST + 14;
    static constexpr int kEditMenuSaveFile  = wxID_HIGHEST + 15;
    static constexpr int kEditMenuWebSearch = wxID_HIGHEST + 16;

    struct SessionUI {
        term::session::SessionId          id       = 0;
        std::string                       label;
        int                               menuId   = 0;
        TerminalTile*                     tile     = nullptr; // wx-parent-owned by grid; non-owning here
        TerminalPanel*                    panel    = nullptr; // wx-child of tile; non-owning here
        std::unique_ptr<SearchController> searchCtrl;
    };

    SessionUI*       FindSessionUI(term::session::SessionId id);
    const SessionUI* FindSessionUI(term::session::SessionId id) const;
    void             UpdateStatusBar();

    void             SetupEditMenu(wxMenu* menu);
    void             PasteFromClipboard();
    bool             HasActiveSelection() const;
    std::u32string   GetFullActiveSelectedText() const;

    term::session::SessionManager& sm_;
    term::input::InputRouter&      router_;
    wxMenu*                        connMenu_;
    MainFrame*                     frame_;
    AppConfig                      cfg_;
    TerminalGrid*                  grid_ = nullptr; // wx-child of frame_; non-owning here
    bool                           firstSession_ = true;

    std::unique_ptr<SelectionActionRegistry> selectionActions_;

    std::unordered_map<term::session::SessionId, SessionUI> sessions_;
    int nextMenuId_ = kMenuIdBase;

    std::mutex                                       pendingRefreshMtx_;
    std::unordered_set<term::session::SessionId>     pendingRefresh_;

    // Set on the UI thread when a session error arrives; displayed in OnSessionDestroyed.
    std::string                                      pendingErrorMsg_;
};

} // namespace ui
