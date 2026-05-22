#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "transport/DisconnectReason.h"
#include "transport/ITransportTarget.h"
#include "transport/TransportError.h"

namespace term::session {

using SessionId = std::size_t;

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionDisconnected(SessionId, transport::DisconnectReason) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionError(SessionId, const term::transport::TransportError& error) = 0;

    // UI thread — called synchronously from SessionManager::CloseSession.
    virtual void OnSessionDestroyed(SessionId) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    // Fired whenever the session enters or exits alt-screen (both program-driven
    // and user-forced via ForceAltScreen).
    virtual void OnAltScreenChanged(SessionId, bool /*active*/) {}

    // Session thread — implementations MUST CallAfter before touching wx.
    // Fired when the terminal receives BEL (\x07). Implementations should
    // trigger a visual bell flash on the associated panel.
    virtual void OnBell(SessionId) {}

    // Session thread — implementations MUST CallAfter before touching wx.
    // Fired once when X11 forwarding becomes active on the SSH channel.
    virtual void OnX11FwdChanged(SessionId, bool /*active*/) {}

    // Session thread — BLOCKS the caller until the user responds.
    // Must dispatch to the UI thread (e.g. wxCallAfter + std::promise/future)
    // and return the user's responses in prompt order.  Returning an empty
    // vector causes authentication failure.
    virtual std::vector<std::string> OnKbdIntChallenge(
        SessionId, const term::transport::KbdIntChallenge& /*challenge*/) { return {}; }
};

} // namespace term::session
