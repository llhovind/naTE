#pragma once

#include <wx/event.h>

wxDECLARE_EVENT(EVT_TERMINAL_ACTION, wxCommandEvent);

enum class TerminalAction
{
    ToggleWrap,
    CloseSession
};

struct TerminalActionData
{
    TerminalAction action;
    long unsigned int sessionId;
};
