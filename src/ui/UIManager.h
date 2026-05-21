#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wx/menu.h>

#include "document/IDocumentListener.h"
#include "transport/TransportError.h"

#include "config/Config.h"
#include "input/InputRouter.h"
#include "session/ISessionObserver.h"
#include "session/SessionManager.h"
#include "ui/ISessionDropTarget.h"
#include "ui/SearchController.h"
#include "ui/SelectionActionRegistry.h"
#include "ui/TerminalActions.h"
#include "ui/TileActions.h"

class MainFrame;
class TerminalPanel;
class TerminalGrid;
class TerminalTile;

namespace ui {

class UIManager : public term::session::ISessionObserver {
public:
    UIManager(term::session::SessionManager& sm,
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
    void OnAltScreenChanged(term::session::SessionId, bool active) override;
    void OnX11FwdChanged(term::session::SessionId, bool active) override;
    void OnBell(term::session::SessionId) override;
    std::vector<std::string> OnKbdIntChallenge(
        term::session::SessionId,
        const term::transport::KbdIntChallenge& challenge) override;

    // -------------------------------------------------------------------------
    // App-initiated session subscription
    // -------------------------------------------------------------------------

    // Subscribe UIManager to an existing session.  Creates a TerminalPanel and
    // registers it as a new tab in targetTile.  When targetTile is nullptr a
    // new TerminalTile is created and added to the grid.
    void TakeSession(term::session::SessionId     id,
                     std::function<std::string()> getTitle,
                     unsigned short               cols,
                     unsigned short               rows,
                     const std::string&           label,
                     TerminalTile*                targetTile = nullptr);

    // Unsubscribe UIManager from a session without closing it (for session moves).
    // Detaches the SessionNotifier, removes the tab, destroys the tile if empty.
    void ReleaseSession(term::session::SessionId id);

    bool HasSession(term::session::SessionId id) const;

    // Close every session this UIManager owns (called by MainFrame::OnClose).
    void CloseAllSessions();

    // Toggle wrap mode for the currently active session.
    void ToggleWrapModeForActive();
    void ToggleWrapModeForSession(term::session::SessionId id);

    // Reset active terminal state (modes/attributes); preserve content.
    void ResetActiveTerminal();
    // Reset and clear all content; prompts to save scrollback first.
    void ResetAndClearActiveTerminal();
    // Save full scrollback of the active session to a user-chosen file.
    void SaveActiveSessionToFile();

    // Open the send-file dialog for the active session (SSH only).
    void SendFilesForActive();
    // Open the file receive dialog for the active session (SSH only).
    void ReceiveFilesForActive();

    // Toggle broadcast mode on/off.
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
    bool HasAnySessions() const { return !sessions_.empty(); }

    // Returns the tile currently hosting the active session (nullptr if none).
    TerminalTile* GetActiveTile() const;

    void MoveActiveSessionToNewTile();
    void MoveActiveSessionToNewWindow();

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

    void SetOnGridEmptyCallback(std::function<void()> cb) { onGridEmptyCb_ = std::move(cb); }
    void SetSessionListChangedCallback(std::function<void()> cb) { onSessionListChanged_ = std::move(cb); }
    void SetOnBeforeCloseCallback(std::function<void()> cb) { onBeforeClose_ = std::move(cb); }

    // Called from MainFrame::OnClose before CloseAllSessions, allowing App to
    // snapshot session state while sessions are still alive.
    void FireBeforeClose() { if (onBeforeClose_) onBeforeClose_(); }

    struct TileSnapshot {
        std::vector<term::session::SessionId> tabOrder;
        int activeTabIndex = -1;
    };

    // Returns one TileSnapshot per tile in grid-insertion order.
    // Must be called on the UI thread; sessions must still be alive.
    std::vector<TileSnapshot> GetTileSnapshots() const;

    // Returns all session IDs and their labels (for Window menu population).
    std::vector<std::pair<term::session::SessionId, std::string>> GetSessionList() const;

    // Returns the first line of the active session's selection (empty if none).
    std::u32string GetActiveSelectedText() const;

    // Force a specific cols×rows on the active session, honouring wrap-mode semantics.
    void SetGeometryForActive(unsigned short cols, unsigned short rows);

    // Current viewport size of the active session; nullopt when none is active.
    std::optional<GeometryPreset> GetActiveGeometry() const;

    // Applies updated config to all open panels immediately (font, colors,
    // padding, web search URL) and stores it for subsequently created panels.
    void UpdateConfig(const AppConfig& cfg);


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
        TerminalTile*                     tile     = nullptr;
        int                               tabIndex = -1;   // slot within tile
        TerminalPanel*                    panel    = nullptr;
        std::unique_ptr<SearchController> searchCtrl;
        std::unique_ptr<SessionNotifier>  notifier;
        bool                              x11Active   = false;
        bool                              altScrActive = false;
    };

    static constexpr int kEditMenuCopy            = wxID_HIGHEST + 10;
    static constexpr int kEditMenuPaste           = wxID_HIGHEST + 11;
    static constexpr int kEditMenuSelectAll       = wxID_HIGHEST + 12;
    static constexpr int kEditMenuPasteSel        = wxID_HIGHEST + 13;
    static constexpr int kEditMenuFind            = wxID_HIGHEST + 14;
    static constexpr int kEditMenuSaveFile        = wxID_HIGHEST + 15;
    static constexpr int kEditMenuWebSearch       = wxID_HIGHEST + 16;
    static constexpr int kEditMenuSaveSessionFile = wxID_HIGHEST + 17;

    SessionUI*       FindSessionUI(term::session::SessionId id);
    const SessionUI* FindSessionUI(term::session::SessionId id) const;
    void             SetupEditMenu(wxMenu* menu);
    void             PasteFromClipboard();
    // Guards multi-line pastes, then routes to the focused session.
    void             DoPaste(const std::string& utf8);
    void             ResizeFrameToFitTiles();
    bool             HasActiveSelection() const;
    std::u32string   GetFullActiveSelectedText() const;

    // Wire tile-level callbacks.  Called once when a tile is first created.
    void WireTileCallbacks(TerminalTile* tile);

    // Common tab / tile teardown logic (called by both OnSessionDestroyed and ReleaseSession).
    void TearDownSessionUI(term::session::SessionId id);

    // Refreshes SetBroadcastActive on all tiles to match current router state.
    void RefreshBroadcastVisuals();

    // Syncs all tile header controls (wrap, alt-scr, x11) and the menu item to
    // the current persisted state of the given session.  Call whenever the
    // active session changes so every indicator reflects the correct session.
    void SyncTileHeaderControls(term::session::SessionId id);

    // Drag handlers.
    // Title-bar drag (OnTileDragStart) moves the whole tile to another window.
    // Tab drag (OnTabDragStart) moves one session to another tile or window.
    void OnTileDragStart(TerminalTile* tile, wxPoint screenAnchor);
    void OnTabDragStart (term::session::SessionId id, wxPoint screenAnchor);
    void OnDragRelease  (wxMouseEvent& evt);

    void OnTerminalAction(TerminalActionEvent& evt);
    void OnTileAction    (TileActionEvent& evt);

    // Handles the "+" new-tab request fired from TabStrip.
    void OnNewTabRequest(TerminalTile* tile);

    term::session::SessionManager& sm_;
    term::input::InputRouter&      router_;
    MainFrame*                     frame_;
    AppConfig                      cfg_;
    TerminalGrid*                  grid_ = nullptr;

    std::unique_ptr<SelectionActionRegistry> selectionActions_;

    std::unordered_map<term::session::SessionId, SessionUI> sessions_;

    term::session::SessionId activeId_ = 0;

    std::mutex                                       pendingRefreshMtx_;
    std::unordered_set<term::session::SessionId>     pendingRefresh_;

    struct DragState {
        std::vector<term::session::SessionId> ids;
        DragIntent    intent  = DragIntent::Tabs;
        TerminalTile* srcTile = nullptr;  // non-null only when intent == DragIntent::Tile
    };
    std::optional<DragState> dragState_;

    std::function<void()> onGridEmptyCb_;
    std::function<void()> onSessionListChanged_;
    std::function<void()> onBeforeClose_;
};

} // namespace ui
