#pragma once
#include "transport/TransportDesc.h"
#include "transport/ITransportTarget.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// Forward-declare libssh2 types.
struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;

namespace term::transport {

// Establishes a ProxyJump / bastion-host tunnel and exposes a local socket fd
// that libssh2 can use as if it were a direct TCP connection to the target.
//
// Lifecycle:
//   auto tun = BastionTunnel::Connect(...);   // blocks until tunnel is ready
//   libssh2_session_handshake(session, tun->LocalFd());  // target SSH over tunnel
//   ... (use target session normally)
//   tun.reset();   // or just let unique_ptr go out of scope
//
// Internally a socketpair(AF_UNIX) is created. A bridge thread continuously
// proxies data between the bastion's direct-tcpip channel and one end of the
// pair; the other end (LocalFd()) is given to the target libssh2 session.
class BastionTunnel {
public:
    // Connect to the jump host, authenticate, and open a direct-tcpip channel
    // to targetHost:targetPort. Throws TransportError on any failure.
    static std::unique_ptr<BastionTunnel> Connect(
        const term::transport::ProxyJumpDesc& jump,
        const std::string&                  targetHost,
        unsigned short                      targetPort,
        const std::string&                  effectiveUser,  // resolved jump user
        int                                 connectTimeoutSec,
        ITransportTarget&                   target);

    // The local fd to pass to libssh2_session_handshake() for the target session.
    // Valid until Stop() or destruction.
    int LocalFd() const { return local_fd_; }

    // Signals and joins the bridge thread, closes fds, frees libssh2 resources.
    // Idempotent.
    void Stop();

    ~BastionTunnel();

    BastionTunnel(const BastionTunnel&)            = delete;
    BastionTunnel& operator=(const BastionTunnel&) = delete;
    BastionTunnel(BastionTunnel&&)                 = delete;
    BastionTunnel& operator=(BastionTunnel&&)      = delete;

private:
    BastionTunnel() = default;

    void BridgeLoop();

    _LIBSSH2_SESSION* bastion_session_  = nullptr;
    _LIBSSH2_CHANNEL* tunnel_channel_  = nullptr;
    int               bastion_sock_    = -1;
    int               bridge_fd_       = -1;  // bridge thread end of socketpair
    int               local_fd_        = -1;  // target session end of socketpair
    std::atomic<bool> stop_            { false };
    std::thread       bridge_thread_;
};

} // namespace term::transport
