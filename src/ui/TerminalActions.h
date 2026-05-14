#pragma once

#include <wx/event.h>
#include "session/ISessionObserver.h"

enum class TerminalAction
{
    ToggleWrap,
    CloseSession
};

// Forward-declare so wxDECLARE_EVENT can reference the type before the class body.
class TerminalActionEvent;
wxDECLARE_EVENT(EVT_TERMINAL_ACTION, TerminalActionEvent);

class TerminalActionEvent : public wxCommandEvent
{
public:
    TerminalActionEvent(TerminalAction action, term::session::SessionId sessionId)
        : wxCommandEvent(EVT_TERMINAL_ACTION)
        , action_(action)
        , sessionId_(sessionId)
    {}

    wxEvent* Clone() const override { return new TerminalActionEvent(*this); }

    TerminalAction           GetAction()    const { return action_; }
    term::session::SessionId GetSessionId() const { return sessionId_; }

private:
    TerminalAction           action_;
    term::session::SessionId sessionId_;
};
