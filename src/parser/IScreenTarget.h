#pragma once

namespace term::parser {

class IScreenTarget {
public:
    virtual ~IScreenTarget() = default;

    virtual void OnEnterAltScreen()              {}
    virtual void OnExitAltScreen()               {}
    virtual void OnSetCursorVisibility(bool /*visible*/)        {}
    virtual void OnSetApplicationCursorKeys(bool /*enabled*/)   {}
    virtual void OnFunctionKey(int /*n*/)        {}
    virtual void OnScrollUp(int /*count*/)       {}
    virtual void OnScrollDown(int /*count*/)     {}
    virtual void OnResetTerminal()               {}
    virtual void OnBell()                        {}
    virtual void OnSetBracketedPaste(bool /*enabled*/) {}
};

} // namespace term::parser
