#pragma once

#include <cstddef>
#include <string>

#include "transport/TransportError.h"

namespace term::session {

using SessionId = std::size_t;

class ISessionObserver {
public:
    virtual ~ISessionObserver() = default;

    // UI thread — called synchronously from SessionManager::CreateSession.
    virtual void OnSessionCreated(SessionId, const std::string& label) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionTitleChanged(SessionId, const std::string& title) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionRefresh(SessionId) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionError(SessionId, const term::transport::TransportError& error) = 0;

    // Session thread — implementations MUST CallAfter before touching wx.
    virtual void OnSessionDisconnected(SessionId) = 0;

    // UI thread — called synchronously from SessionManager::CloseSession.
    virtual void OnSessionDestroyed(SessionId) = 0;
};

} // namespace term::session
