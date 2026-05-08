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
};

} // namespace term::transport
