#pragma once

#include <cstddef>

#include "transport/TransportError.h"

namespace term::session {

using SessionId = std::size_t;

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionDisconnected(SessionId) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionError(SessionId, const term::transport::TransportError& error) = 0;

    // UI thread — called synchronously from SessionManager::CloseSession.
    virtual void OnSessionDestroyed(SessionId) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    // Fired whenever the session enters or exits alt-screen (both program-driven
    // and user-forced via ForceAltScreen).
    virtual void OnAltScreenChanged(SessionId, bool /*active*/) {}
};

} // namespace term::session
