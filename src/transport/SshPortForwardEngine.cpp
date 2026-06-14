#include "transport/SshPortForwardEngine.h"
#include "transport/ITransportTarget.h"
#include "transport/SshSession.h"   // ssh::PollUntilReady, ssh::LastSshError, kPollTimeoutMs

#include <libssh2.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace term::transport {

namespace {
// Pending-connection queue depth for forwarded listeners (local accept() and
// the remote libssh2 forward listener share the same semantics).
constexpr int kListenBacklog = 16;
}  // namespace

PortForwardEngine::PortForwardEngine(_LIBSSH2_SESSION*& session, int& sockFd,
                                     const std::atomic<bool>& running,
                                     ITransportTarget& target)
    : session_(session), sockFd_(sockFd), running_(running), target_(target)
{}

bool PortForwardEngine::PollUntilReady()
{
    return ssh::PollUntilReady(session_, sockFd_, ssh::kPollTimeoutMs, running_);
}

std::string PortForwardEngine::ErrnoString(int err)
{
    char buf[256];
    return ::strerror_r(err, buf, sizeof(buf));
}

// ---------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------

void PortForwardEngine::AddForward(const PortForwardDesc& desc)
{
    std::lock_guard<std::mutex> lk(mutex_);
    pending_.push_back(PfwAdd{desc});
}

void PortForwardEngine::RemoveForward(PortForwardId id)
{
    std::lock_guard<std::mutex> lk(mutex_);
    pending_.push_back(PfwRemove{id});
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

void PortForwardEngine::NotifyStatus()
{
    std::vector<PortForwardStatus> status;
    for (const auto& fwd : localFwds_) {
        PortForwardStatus s;
        s.id          = fwd.desc.id;
        s.active      = fwd.listen_fd >= 0;
        s.connections = fwd.conns.size();
        status.push_back(std::move(s));
    }
    for (const auto& fwd : remoteFwds_) {
        PortForwardStatus s;
        s.id          = fwd.desc.id;
        s.active      = fwd.listener != nullptr;
        s.connections = fwd.conns.size();
        status.push_back(std::move(s));
    }
    // Append persisted failures so they remain visible until explicitly removed.
    for (const auto& [id, error] : failed_) {
        PortForwardStatus s;
        s.id     = id;
        s.active = false;
        s.error  = error;
        status.push_back(std::move(s));
    }

    if (status == lastStatus_) return;
    lastStatus_ = status;
    target_.OnPortForwardStatusChanged(std::move(status));
}

void PortForwardEngine::FireFailed(PortForwardId id, const std::string& error)
{
    failed_[id] = error;
    NotifyStatus();
}

// ---------------------------------------------------------------------------
// Pending add/remove queue
// ---------------------------------------------------------------------------

void PortForwardEngine::ServiceQueue()
{
    std::vector<PfwPending> pending;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending.swap(pending_);
    }
    if (pending.empty()) return;

    bool changed = false;
    for (auto& item : pending) {
        std::visit([&](auto& v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, PfwAdd>) {
                const PortForwardDesc& desc = v.desc;

                if (desc.direction == PortForwardDirection::Local) {
                    // Resolve bindAddr and create the listening socket.
                    const std::string portStr = std::to_string(desc.localPort);
                    addrinfo hints{};
                    hints.ai_family   = AF_UNSPEC;
                    hints.ai_socktype = SOCK_STREAM;
                    hints.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;
                    const char* host  = desc.bindAddr.empty() ? nullptr
                                                              : desc.bindAddr.c_str();
                    addrinfo* res = nullptr;
                    if (::getaddrinfo(host, portStr.c_str(), &hints, &res) != 0 || !res) {
                        FireFailed(desc.id, "getaddrinfo: " + ErrnoString(errno));
                        return;
                    }
                    int fd = ::socket(res->ai_family, SOCK_STREAM, 0);
                    if (fd < 0) {
                        ::freeaddrinfo(res);
                        FireFailed(desc.id, "socket: " + ErrnoString(errno));
                        return;
                    }
                    int on = 1;
                    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
                    if (::bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
                        const std::string msg = "bind: " + ErrnoString(errno);
                        ::close(fd);
                        ::freeaddrinfo(res);
                        FireFailed(desc.id, msg);
                        return;
                    }
                    ::freeaddrinfo(res);
                    ::listen(fd, kListenBacklog);
                    ::fcntl(fd, F_SETFL, O_NONBLOCK);
                    localFwds_.push_back({desc, fd, {}});
                    changed = true;

                } else {
                    // Remote forward: ask the SSH server to listen on a port.
                    int bound = 0;
                    _LIBSSH2_LISTENER* lst = nullptr;
                    while (running_) {
                        lst = libssh2_channel_forward_listen_ex(
                                session_,
                                desc.bindAddr.c_str(),
                                static_cast<int>(desc.remotePort),
                                &bound, kListenBacklog);
                        if (lst) break;
                        const int err = libssh2_session_last_error(
                                session_, nullptr, nullptr, 0);
                        if (err != LIBSSH2_ERROR_EAGAIN) {
                            FireFailed(desc.id, "forward-listen: " + ssh::LastSshError(session_));
                            return;
                        }
                        if (!PollUntilReady()) return;
                    }
                    if (!lst) return;
                    ActiveRemoteFwd rfwd;
                    rfwd.desc       = desc;
                    rfwd.listener   = lst;
                    rfwd.bound_port = bound;
                    remoteFwds_.push_back(std::move(rfwd));
                    changed = true;
                }

            } else if constexpr (std::is_same_v<T, PfwRemove>) {
                const PortForwardId id = v.id;

                // Remove from local forwards.
                auto lit = std::find_if(localFwds_.begin(), localFwds_.end(),
                                        [id](const ActiveLocalFwd& f) { return f.desc.id == id; });
                if (lit != localFwds_.end()) {
                    ::close(lit->listen_fd);
                    ReleaseAllProxies(lit->conns);
                    localFwds_.erase(lit);
                    failed_.erase(id);
                    changed = true;
                    return;
                }

                // Remove from remote forwards.
                auto rit = std::find_if(remoteFwds_.begin(), remoteFwds_.end(),
                                        [id](const ActiveRemoteFwd& f) { return f.desc.id == id; });
                if (rit != remoteFwds_.end()) {
                    libssh2_channel_forward_cancel(rit->listener);
                    ReleaseAllProxies(rit->conns);
                    remoteFwds_.erase(rit);
                    failed_.erase(id);
                    changed = true;
                    return;
                }

                // May be in failed_ only (setup never succeeded).
                if (failed_.erase(id)) changed = true;
            }
        }, item);
    }

    if (changed) NotifyStatus();
}

// ---------------------------------------------------------------------------
// Poll integration
// ---------------------------------------------------------------------------

void PortForwardEngine::AppendPollFds(std::vector<pollfd>& pfds)
{
    pollBase_ = pfds.size();
    pollTags_.clear();

    // Listen sockets for local forwards.
    for (size_t i = 0; i < localFwds_.size(); ++i) {
        pfds.push_back({localFwds_[i].listen_fd, POLLIN, 0});
        pollTags_.push_back({PfwPollEntry::Kind::LocalListen, i, 0});
    }
    // Proxy connection local sockets for local forwards.
    for (size_t i = 0; i < localFwds_.size(); ++i) {
        for (size_t j = 0; j < localFwds_[i].conns.size(); ++j) {
            const auto& c = localFwds_[i].conns[j];
            pfds.push_back({c.closed ? -1 : c.local_fd, POLLIN, 0});
            pollTags_.push_back({PfwPollEntry::Kind::LocalConn, i, j});
        }
    }
    // Proxy connection local sockets for remote forwards.
    for (size_t i = 0; i < remoteFwds_.size(); ++i) {
        for (size_t j = 0; j < remoteFwds_[i].conns.size(); ++j) {
            const auto& c = remoteFwds_[i].conns[j];
            pfds.push_back({c.closed ? -1 : c.local_fd, POLLIN, 0});
            pollTags_.push_back({PfwPollEntry::Kind::RemoteConn, i, j});
        }
    }
}

void PortForwardEngine::ServiceConns(const std::vector<pollfd>& pfds,
                                     char* buf, size_t bufLen)
{
    // --- Process tagged poll entries (accept + local→SSH data) ---------------
    for (size_t t = 0; t < pollTags_.size(); ++t) {
        const size_t pfdIdx = pollBase_ + t;
        const auto&  tag    = pollTags_[t];
        const short  rev    = pfds[pfdIdx].revents;

        if (tag.kind == PfwPollEntry::Kind::LocalListen) {
            if (!(rev & POLLIN)) continue;
            auto& fwd  = localFwds_[tag.fwdIdx];
            int   conn = ::accept(fwd.listen_fd, nullptr, nullptr);
            if (conn < 0) continue;
            ::fcntl(conn, F_SETFL, O_NONBLOCK);

            _LIBSSH2_CHANNEL* ch = nullptr;
            while (running_) {
                ch = libssh2_channel_direct_tcpip_ex(
                        session_,
                        fwd.desc.remoteHost.c_str(),
                        static_cast<int>(fwd.desc.remotePort),
                        "127.0.0.1",
                        static_cast<int>(fwd.desc.localPort));
                if (ch) break;
                const int err = libssh2_session_last_error(session_, nullptr, nullptr, 0);
                if (err != LIBSSH2_ERROR_EAGAIN) { ch = nullptr; break; }
                if (!PollUntilReady()) { ch = nullptr; break; }
            }
            if (ch) fwd.conns.push_back({.channel = ch, .local_fd = conn});
            else    ::close(conn);  // connection-time failure; forward stays live

        } else {
            // LocalConn or RemoteConn: local→SSH data.
            ChannelProxy& c = (tag.kind == PfwPollEntry::Kind::LocalConn)
                              ? localFwds_[tag.fwdIdx].conns[tag.connIdx]
                              : remoteFwds_[tag.fwdIdx].conns[tag.connIdx];
            c.PumpLocalToChannel(rev, buf, bufLen);
        }
    }

    // --- Accept new remote-forward connections (SSH-side, non-blocking) ------
    for (auto& fwd : remoteFwds_) {
        _LIBSSH2_CHANNEL* ch = libssh2_channel_forward_accept(fwd.listener);
        if (!ch) continue;

        // Resolve the local connect target and create the socket.
        const std::string portStr  = std::to_string(fwd.desc.localPort);
        const char*       host     = fwd.desc.remoteHost.empty()
                                     ? "127.0.0.1" : fwd.desc.remoteHost.c_str();
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags    = AI_NUMERICSERV;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host, portStr.c_str(), &hints, &res) != 0 || !res) {
            libssh2_channel_free(ch);
            continue;
        }
        int  conn = ::socket(res->ai_family, SOCK_STREAM, 0);
        bool ok   = false;
        if (conn >= 0) {
            ok = (::connect(conn, res->ai_addr, res->ai_addrlen) == 0 ||
                  errno == EINPROGRESS);
            if (!ok) { ::close(conn); conn = -1; }
        }
        ::freeaddrinfo(res);
        if (!ok || conn < 0) { libssh2_channel_free(ch); continue; }
        ::fcntl(conn, F_SETFL, O_NONBLOCK);
        fwd.conns.push_back({.channel = ch, .local_fd = conn});
    }

    // --- SSH→local, then sweep closed proxy connections ----------------------
    bool connChanged = false;
    for (auto& fwd : localFwds_) {
        PumpChannelsToLocal(fwd.conns, buf, bufLen);
        connChanged |= SweepClosedProxies(fwd.conns);
    }
    for (auto& fwd : remoteFwds_) {
        PumpChannelsToLocal(fwd.conns, buf, bufLen);
        connChanged |= SweepClosedProxies(fwd.conns);
    }

    if (connChanged) NotifyStatus();
}

void PortForwardEngine::Teardown()
{
    for (auto& fwd : localFwds_) {
        ::close(fwd.listen_fd);
        ReleaseAllProxies(fwd.conns);
    }
    localFwds_.clear();
    for (auto& fwd : remoteFwds_) {
        libssh2_channel_forward_cancel(fwd.listener);
        ReleaseAllProxies(fwd.conns);
    }
    remoteFwds_.clear();
}

} // namespace term::transport
