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
    std::string   error;    // non-empty = persistent failure (e.g. forwarding prohibited)
    std::string   warning;  // non-empty = transient issue (e.g. target unreachable)

    bool operator==(const PortForwardStatus& o) const
    {
        return id == o.id && active == o.active &&
               connections == o.connections && error == o.error &&
               warning == o.warning;
    }
};

// Outcome of attempting to open a direct-tcpip channel for a local (-L) forward.
// Prohibited is a persistent server-policy denial (AllowTcpForwarding no);
// ConnectFailed is a target-side failure that may clear once the service starts.
enum class DirectOpenKind : uint8_t { Ok, Prohibited, ConnectFailed, Aborted };

// Pure classification of a libssh2 channel-open error message. Header-inline and
// dependency-free so it is unit-testable without a live SSH session. libssh2
// embeds the SSH reason code in the message text (see src/channel.c), e.g.
// "Channel open failure (administratively prohibited)".
inline DirectOpenKind ClassifyDirectOpenError(const std::string& message)
{
    if (message.find("administratively prohibited") != std::string::npos)
        return DirectOpenKind::Prohibited;
    return DirectOpenKind::ConnectFailed;
}

} // namespace term::transport
