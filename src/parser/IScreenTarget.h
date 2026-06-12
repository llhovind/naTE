#pragma once
#include <string>

namespace term::parser {

class IScreenTarget {
public:
    virtual ~IScreenTarget() = default;

    virtual void OnEnterAltScreen()              {}
    virtual void OnExitAltScreen()               {}
    // Fired when the shell emits an OSC 7 working-directory notification.
    virtual void OnCwdChanged(const std::string& /*path*/) {}
    virtual void OnSetCursorVisibility(bool /*visible*/)        {}
    virtual void OnSetApplicationCursorKeys(bool /*enabled*/)   {}
    virtual void OnResetTerminal()               {}
    virtual void OnBell()                        {}
    virtual void OnSetBracketedPaste(bool /*enabled*/) {}
    virtual void OnDeviceStatusReport(int /*param*/)   {}  // CSI n — reply expected for param==6
};

} // namespace term::parser
