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
        s.error       = fwd.error;
        s.warning     = fwd.warning;
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
// direct-tcpip channel open (probe + per-connection)
// ---------------------------------------------------------------------------

PortForwardEngine::DirectOpenResult
PortForwardEngine::OpenDirectTcpip(const PortForwardDesc& desc)
{
    while (running_) {
        _LIBSSH2_CHANNEL* ch = libssh2_channel_direct_tcpip_ex(
                session_,
                desc.remoteHost.c_str(),
                static_cast<int>(desc.remotePort),
                "127.0.0.1",
                static_cast<int>(desc.localPort));
        if (ch) return {DirectOpenKind::Ok, ch, {}};

        const int err = libssh2_session_last_error(session_, nullptr, nullptr, 0);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            std::string msg = ssh::LastSshError(session_);
            return {ClassifyDirectOpenError(msg), nullptr, std::move(msg)};
        }
        if (!PollUntilReady()) break;
    }
    return {DirectOpenKind::Aborted, nullptr, {}};
}

void PortForwardEngine::FreeChannel(_LIBSSH2_CHANNEL* channel)
{
    if (!channel) return;
    // Bound the EAGAIN loop: a freshly opened probe channel frees quickly, and a
    // stuck session must not pin the worker thread here.
    for (int i = 0; i < 64 && running_; ++i) {
        if (libssh2_channel_free(channel) != LIBSSH2_ERROR_EAGAIN) return;
        if (!PollUntilReady()) return;
    }
}

void PortForwardEngine::ProbeLocalForward(ActiveLocalFwd& fwd)
{
    DirectOpenResult r = OpenDirectTcpip(fwd.desc);
    switch (r.kind) {
        case DirectOpenKind::Ok:
            FreeChannel(r.channel);  // throwaway probe channel
            break;
        case DirectOpenKind::Prohibited:
            // Server policy forbids forwarding: the local listener can never
            // service a connection, so retire it and surface a hard error.
            ::close(fwd.listen_fd);
            fwd.listen_fd = -1;
            fwd.error     = r.message;
            break;
        case DirectOpenKind::ConnectFailed:
            // Forwarding works; the target just isn't reachable yet. Soft
            // warning that clears on the first successful connection.
            fwd.warning = r.message;
            break;
        case DirectOpenKind::Aborted:
            break;  // session tearing down — leave state untouched
    }
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
                    localFwds_.push_back(ActiveLocalFwd{desc, fd, {}, {}, {}});
                    // Probe immediately: a -L listener binds locally and reports
                    // success regardless of the server's AllowTcpForwarding
                    // policy, so a prohibited/refused forward would otherwise
                    // stay silent until first use.
                    ProbeLocalForward(localFwds_.back());
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
    bool changed = false;  // any status-affecting change → NotifyStatus at end

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

            DirectOpenResult r = OpenDirectTcpip(fwd.desc);
            switch (r.kind) {
                case DirectOpenKind::Ok:
                    if (!fwd.warning.empty()) { fwd.warning.clear(); changed = true; }
                    fwd.conns.push_back({.channel = r.channel, .local_fd = conn});
                    changed = true;
                    break;
                case DirectOpenKind::Prohibited:
                    // Forwarding was revoked since setup: retire the listener.
                    ::close(conn);
                    ::close(fwd.listen_fd);
                    fwd.listen_fd = -1;
                    fwd.error     = r.message;
                    changed       = true;
                    break;
                case DirectOpenKind::ConnectFailed:
                    ::close(conn);
                    if (fwd.warning != r.message) { fwd.warning = r.message; changed = true; }
                    break;
                case DirectOpenKind::Aborted:
                    ::close(conn);  // session tearing down
                    break;
            }

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
    for (auto& fwd : localFwds_) {
        PumpChannelsToLocal(fwd.conns, buf, bufLen);
        changed |= SweepClosedProxies(fwd.conns);
    }
    for (auto& fwd : remoteFwds_) {
        PumpChannelsToLocal(fwd.conns, buf, bufLen);
        changed |= SweepClosedProxies(fwd.conns);
    }

    if (changed) NotifyStatus();
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
