#pragma once
#include <cstdint>
#include <string>

namespace term::transport {

using PortForwardId = uint32_t;  // runtime identity; 0 = unassigned

enum class PortForwardDirection { Local, Remote };

struct PortForwardDesc {
    PortForwardId        id         = 0;
    PortForwardDirection direction  = PortForwardDirection::Local;
    std::string          bindAddr   = "127.0.0.1";  // local bind (Local) or server bind (Remote)
    uint16_t             localPort  = 0;
    std::string          remoteHost = "localhost";   // Local: target host; Remote: local connect host
    uint16_t             remotePort = 0;
    std::string          label;
};

struct PortForwardStatus {
    PortForwardId id;
    bool          active      = false;
    size_t        connections = 0;
    std::string   error;  // non-empty = setup failed

    bool operator==(const PortForwardStatus& o) const
    {
        return id == o.id && active == o.active &&
               connections == o.connections && error == o.error;
    }
};

} // namespace term::transport
