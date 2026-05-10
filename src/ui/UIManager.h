#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <wx/menu.h>

#include "document/IDocumentListener.h"
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
class wxGenericDragImage;

namespace ui {

class UIManager : public term::session::ISessionObserver {
public:
    UIManager(term::session::SessionManager& sm,
              wxMenu*                        connMenu,
              MainFrame*                     frame,
              const AppConfig&               cfg,
              term::input::InputRouter&      router,
              wxMenu*                        editMenu);

    ~UIManager() override;

    // -------------------------------------------------------------------------
    // ISessionObserver — 3-method slim interface
    // -------------------------------------------------------------------------
    void OnSessionDisconnected(term::session::SessionId) override;
    void OnSessionError(term::session::SessionId,
                        const term::transport::TransportError& error) override;
    void OnSessionDestroyed(term::session::SessionId) override;

    // -------------------------------------------------------------------------
    // App-initiated session subscription (replaces ISessionObserver::OnSessionCreated)
    // -------------------------------------------------------------------------

    // Subscribe UIManager to an existing session: creates tile + SessionNotifier
    // and attaches the notifier. cols is a hint for the initial tile width.
    void TakeSession(term::session::SessionId  id,
                     std::function<std::string()> getTitle,
                     unsigned short            cols,
                     const std::string&        label);

    // Unsubscribe UIManager from a session without closing it (for session moves).
    // Detaches the SessionNotifier, destroys the tile, auto-activates next session.
    void ReleaseSession(term::session::SessionId id);

    // Close every session this UIManager owns (called by MainFrame::OnClose).
    void CloseAllSessions();

    // Toggle wrap mode for the currently active session.
    void TogglewrapModeForActive();

    // Toggle broadcast mode on/off.  Enabling with an empty selection auto-selects
    // all sessions in this window.  Disabling preserves the selection for restore.
    void ToggleBroadcastMode();

    // Toggle one session's membership in the broadcast group.
    void ToggleTileBroadcast(term::session::SessionId id);

    bool IsBroadcastMode() const {
        return router_.GetMode() == term::input::InputMode::Broadcast;
    }

    // -------------------------------------------------------------------------
    // UI-thread event routing
    // -------------------------------------------------------------------------

    void RequestActivate(term::session::SessionId id);

    term::session::SessionId GetActiveSessionId() const { return activeId_; }

    // Routed from TerminalPanel callbacks — always on the UI thread.
    void OnScroll(term::session::SessionId id, int topRow);
    void OnViewportResize(term::session::SessionId id,
                          unsigned short cols, unsigned short rows);

    // Called on keyboard input to snap the active session's viewport to the cursor.
    void EnsureCursorVisibleForActive();

    // Search bar access.
    SearchController* GetActiveSearchController();
    bool              SearchBarHasFocus() const;
    void              ShowSearchBarForActive(bool show,
                                             const std::u32string& initialQuery = {});

    // Returns the first line of the active session's selection (empty if none).
    std::u32string GetActiveSelectedText() const;

    // -------------------------------------------------------------------------
    // Drag-to-move callback (set by App in CreateNewWindow)
    // -------------------------------------------------------------------------
    using MoveSessionCallback = std::function<void(term::session::SessionId, MainFrame*)>;
    void SetMoveSessionCallback(MoveSessionCallback cb) { moveSessionCb_ = std::move(cb); }

private:
    // -------------------------------------------------------------------------
    // Per-session document listener — owned by SessionUI, runs on session thread.
    // -------------------------------------------------------------------------
    struct SessionNotifier : public IDocumentListener {
        term::session::SessionId     id;
        UIManager*                   mgr;
        std::function<std::string()> getTitle;
        void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;
    };

    // -------------------------------------------------------------------------
    // Per-session UI state
    // -------------------------------------------------------------------------
    struct SessionUI {
        term::session::SessionId          id       = 0;
        std::string                       label;
        int                               menuId   = 0;
        TerminalTile*                     tile     = nullptr;
        TerminalPanel*                    panel    = nullptr;
        std::unique_ptr<SearchController> searchCtrl;
        std::unique_ptr<SessionNotifier>  notifier;
    };

    static constexpr int kMenuIdBase        = wxID_HIGHEST + 200;
    static constexpr int kEditMenuCopy      = wxID_HIGHEST + 10;
    static constexpr int kEditMenuPaste     = wxID_HIGHEST + 11;
    static constexpr int kEditMenuSelectAll = wxID_HIGHEST + 12;
    static constexpr int kEditMenuPasteSel  = wxID_HIGHEST + 13;
    static constexpr int kEditMenuFind      = wxID_HIGHEST + 14;
    static constexpr int kEditMenuSaveFile  = wxID_HIGHEST + 15;
    static constexpr int kEditMenuWebSearch = wxID_HIGHEST + 16;

    SessionUI*       FindSessionUI(term::session::SessionId id);
    const SessionUI* FindSessionUI(term::session::SessionId id) const;
    void             UpdateStatusBar();
    void             SetupEditMenu(wxMenu* menu);
    void             PasteFromClipboard();
    void             ResizeFrameToFitTiles();
    bool             HasActiveSelection() const;
    std::u32string   GetFullActiveSelectedText() const;

    // Common tile teardown logic (called by both OnSessionDestroyed and ReleaseSession).
    void TearDownSessionUI(term::session::SessionId id);

    // Refreshes SetBroadcastActive on all tiles to match current router state.
    void RefreshBroadcastVisuals();

    // Drag handlers
    void OnTileDragStart(term::session::SessionId id, wxPoint screenAnchor);
    void OnDragMotion(wxMouseEvent& evt);
    void OnDragRelease(wxMouseEvent& evt);

    term::session::SessionManager& sm_;
    term::input::InputRouter&      router_;
    wxMenu*                        connMenu_;
    MainFrame*                     frame_;
    AppConfig                      cfg_;
    TerminalGrid*                  grid_ = nullptr;

    std::unique_ptr<SelectionActionRegistry> selectionActions_;

    std::unordered_map<term::session::SessionId, SessionUI> sessions_;
    int nextMenuId_ = kMenuIdBase;

    term::session::SessionId activeId_ = 0;

    std::mutex                                       pendingRefreshMtx_;
    std::unordered_set<term::session::SessionId>     pendingRefresh_;

    std::string                                      pendingErrorMsg_;

    MoveSessionCallback                              moveSessionCb_;

    // Drag state
    std::unique_ptr<wxGenericDragImage>              dragImage_;
    term::session::SessionId                         draggingId_ = 0;
};

} // namespace ui
