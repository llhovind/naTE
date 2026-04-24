#pragma once
#include <string>

namespace term::transport {

class ITransportTarget {
public:
    virtual ~ITransportTarget() = default;
    virtual void OnData(const std::string& data) = 0;
    virtual void OnDisconnect() = 0;
};

} // namespace term::transport
