#pragma once
#include <cstddef>
#include <vector>

struct _LIBSSH2_CHANNEL;

namespace term::transport {

// A bidirectional byte pump between a local socket and an SSH channel.
// X11 forwarding, agent forwarding, and port-forward connections are
// identical at this level — one struct replaces the three that used to be
// maintained in parallel. Worker-thread-only: libssh2 channels must only be
// touched from the thread that owns the session.
struct ChannelProxy {
    _LIBSSH2_CHANNEL* channel  = nullptr;
    int               local_fd = -1;   // socket to the local peer (X server, agent, TCP client)
    bool              closed   = false;

    // Local socket → SSH channel, driven by the socket's poll() revents.
    // POLLHUP/POLLERR or a failed read marks the proxy closed. The channel
    // write is best-effort: EAGAIN / a partial send is tolerated, matching
    // the historical inline implementations.
    void PumpLocalToChannel(short revents, char* buf, size_t bufLen);

    // SSH channel → local socket: drains until EAGAIN. EOF, a channel error,
    // or a failed local write marks the proxy closed.
    void PumpChannelToLocal(char* buf, size_t bufLen);
};

// Drains every open proxy channel → local socket.
void PumpChannelsToLocal(std::vector<ChannelProxy>& proxies, char* buf, size_t bufLen);

// Releases (close + libssh2_channel_free) and erases proxies marked closed.
// Resource release happens before the erase so the remove_if predicate stays
// side-effect free. Returns true when at least one proxy was removed.
bool SweepClosedProxies(std::vector<ChannelProxy>& proxies);

// Unconditionally releases every proxy and clears the vector (teardown path —
// also covers proxies marked closed after the last sweep of the I/O loop).
void ReleaseAllProxies(std::vector<ChannelProxy>& proxies);

} // namespace term::transport
