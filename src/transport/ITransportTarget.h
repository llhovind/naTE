#pragma once
#include <string>

#include "transport/TransportError.h"

namespace term::transport {

class ITransportTarget {
public:
    virtual ~ITransportTarget() = default;
    virtual void OnData(const std::string& data) = 0;
    virtual void OnError(const TransportError& error) = 0;
    virtual void OnDisconnect() = 0;

    // Fired on the worker thread when X11 forwarding becomes active on a channel.
    // Implementations must CallAfter before touching wx objects.
    virtual void OnX11StateChanged(bool /*active*/) {}
};

} // namespace term::transport
