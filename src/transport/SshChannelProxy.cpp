#include "transport/SshChannelProxy.h"

#include <libssh2.h>

#include <algorithm>
#include <poll.h>
#include <unistd.h>

namespace term::transport {

void ChannelProxy::PumpLocalToChannel(short revents, char* buf, size_t bufLen)
{
    if (closed) return;
    if (revents & (POLLHUP | POLLERR)) { closed = true; return; }
    if (!(revents & POLLIN))             return;

    const ssize_t n = ::read(local_fd, buf, bufLen);
    if (n <= 0) { closed = true; return; }
    libssh2_channel_write(channel, buf, static_cast<size_t>(n));
}

void ChannelProxy::PumpChannelToLocal(char* buf, size_t bufLen)
{
    if (closed) return;
    ssize_t n;
    while ((n = libssh2_channel_read(channel, buf, bufLen)) > 0) {
        if (::write(local_fd, buf, static_cast<size_t>(n)) < 0) {
            closed = true;
            return;
        }
    }
    if (n == 0 || (n < 0 && n != LIBSSH2_ERROR_EAGAIN) ||
        libssh2_channel_eof(channel))
        closed = true;
}

void PumpChannelsToLocal(std::vector<ChannelProxy>& proxies, char* buf, size_t bufLen)
{
    for (auto& p : proxies)
        p.PumpChannelToLocal(buf, bufLen);
}

bool SweepClosedProxies(std::vector<ChannelProxy>& proxies)
{
    const size_t before = proxies.size();
    for (auto& p : proxies) {
        if (!p.closed) continue;
        if (p.local_fd >= 0) ::close(p.local_fd);
        if (p.channel)       libssh2_channel_free(p.channel);
    }
    proxies.erase(
        std::remove_if(proxies.begin(), proxies.end(),
                       [](const ChannelProxy& p) { return p.closed; }),
        proxies.end());
    return proxies.size() != before;
}

void ReleaseAllProxies(std::vector<ChannelProxy>& proxies)
{
    for (auto& p : proxies) {
        if (p.local_fd >= 0) ::close(p.local_fd);
        if (p.channel)       libssh2_channel_free(p.channel);
    }
    proxies.clear();
}

} // namespace term::transport
