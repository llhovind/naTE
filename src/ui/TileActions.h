#pragma once

#include <wx/event.h>
#include "session/ISessionObserver.h"

class TerminalTile;

// Tile-level actions emitted by TerminalTile and handled by UIManager.
// Kept separate from TerminalAction, which is scoped to Session-level operations.
enum class TileAction {
    CloseTab,    // close a specific tab's session within this tile
    NewTabHere,  // open a new connection as a tab in this tile
};

class TileActionEvent;
wxDECLARE_EVENT(EVT_TILE_ACTION, TileActionEvent);

class TileActionEvent : public wxCommandEvent {
public:
    TileActionEvent(TileAction action,
                    term::session::SessionId sessionId,
                    TerminalTile* tile)
        : wxCommandEvent(EVT_TILE_ACTION)
        , action_(action)
        , sessionId_(sessionId)
        , tile_(tile)
    {}

    wxEvent* Clone() const override { return new TileActionEvent(*this); }

    TileAction                GetAction()    const { return action_; }
    term::session::SessionId  GetSessionId() const { return sessionId_; }
    TerminalTile*             GetTile()      const { return tile_; }

private:
    TileAction                action_;
    term::session::SessionId  sessionId_;
    TerminalTile*             tile_;
};
