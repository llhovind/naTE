#include "transport/BastionTunnel.h"
#include "transport/SshPublicKey.h"
#include "transport/TransportError.h"

#include <libssh2.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace term::transport {

namespace {

constexpr int  kBridgePollMs = 200;
constexpr char kKnownHostsSubdir[] = "/.nate/known_hosts";

std::string BastionKnownHostsPath()
{
    const char* home = ::getenv("HOME");
    if (!home || home[0] == '\0') home = "/tmp";
    const std::string dir = std::string(home) + "/.nate";
    ::mkdir(dir.c_str(), 0700);
    return dir + "/known_hosts";
}

// TCP connect (blocking) to host:port with optional timeout.
// Returns a connected fd, or -1 and sets outErr on failure.
int TcpConnect(const std::string& host, unsigned short port,
               int timeoutSec, std::string& outErr)
{
    const std::string portStr = std::to_string(port);
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        outErr = "SSH ProxyJump: name resolution failed for " + host;
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (timeoutSec > 0) {
            timeval tv{};
            tv.tv_sec = timeoutSec;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0)
        outErr = "SSH ProxyJump: could not connect to " + host + ":" + portStr;
    return fd;
}

// TOFU host-key check against ~/.nate/known_hosts.
// Returns false and sets outErr on mismatch. Silently adds unknown keys.
bool VerifyBastionHostKey(_LIBSSH2_SESSION* session,
                          const std::string& host, unsigned short port,
                          std::string& outErr)
{
    size_t keyLen  = 0;
    int    keyType = 0;
    const char* key = libssh2_session_hostkey(session, &keyLen, &keyType);
    if (!key) {
        outErr = "SSH ProxyJump: could not retrieve host key for " + host;
        return false;
    }

    LIBSSH2_KNOWNHOSTS* kh = libssh2_knownhost_init(session);
    if (!kh) {
        outErr = "SSH ProxyJump: libssh2_knownhost_init failed";
        return false;
    }

    const std::string khPath = BastionKnownHostsPath();
    libssh2_knownhost_readfile(kh, khPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    int typeFlag = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (keyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:       typeFlag |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;     break;
        case LIBSSH2_HOSTKEY_TYPE_DSS:       typeFlag |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;     break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ECDSA_256;  break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ECDSA_384;  break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ECDSA_521;  break;
        case LIBSSH2_HOSTKEY_TYPE_ED25519:   typeFlag |= LIBSSH2_KNOWNHOST_KEY_ED25519;    break;
        default:                             typeFlag |= LIBSSH2_KNOWNHOST_KEY_UNKNOWN;     break;
    }

    libssh2_knownhost* found = nullptr;
    const int check = libssh2_knownhost_checkp(
        kh, host.c_str(), static_cast<int>(port),
        key, keyLen, typeFlag, &found);

    bool ok = true;
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        libssh2_knownhost_addc(kh, host.c_str(), nullptr, key, keyLen,
                               nullptr, 0, typeFlag, nullptr);
        libssh2_knownhost_writefile(kh, khPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    } else if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        outErr = "SSH ProxyJump: HOST KEY MISMATCH for " + host +
                 " — possible man-in-the-middle attack.\n"
                 "Remove the entry from " + khPath + " to proceed.";
        ok = false;
    }

    libssh2_knownhost_free(kh);
    return ok;
}

// Try all agent identities (blocking). Returns true on first accepted.
bool AgentTryAll(_LIBSSH2_AGENT* agent, const char* user)
{
    libssh2_agent_publickey* id   = nullptr;
    libssh2_agent_publickey* prev = nullptr;
    while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
        if (libssh2_agent_userauth(agent, user, id) == 0) return true;
        prev = id;
    }
    return false;
}

// Try preferred blobs first, then fall back to all identities.
bool AgentAuthBlocking(_LIBSSH2_SESSION* session, const char* user,
                       const std::vector<std::vector<uint8_t>>& preferred)
{
    LIBSSH2_AGENT* agent = libssh2_agent_init(session);
    if (!agent) return false;

    bool ok = false;
    if (libssh2_agent_connect(agent) == 0 &&
        libssh2_agent_list_identities(agent) == 0) {
        if (!preferred.empty()) {
            libssh2_agent_publickey* id   = nullptr;
            libssh2_agent_publickey* prev = nullptr;
            while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
                const bool matches = std::any_of(
                    preferred.begin(), preferred.end(),
                    [&](const std::vector<uint8_t>& b) {
                        return b.size() == id->blob_len &&
                               std::memcmp(b.data(), id->blob, b.size()) == 0;
                    });
                if (matches && libssh2_agent_userauth(agent, user, id) == 0) {
                    ok = true; break;
                }
                prev = id;
            }
            if (!ok) ok = AgentTryAll(agent, user);
        } else {
            ok = AgentTryAll(agent, user);
        }
    }
    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);
    return ok;
}

// Context for the kbd-interactive callback used during bastion auth.
struct BastionKbdIntCtx {
    term::transport::ITransportTarget* target;
};

void BastionKbdIntCallback(
    const char* name,        int name_len,
    const char* instruction, int instruction_len,
    int num_prompts,
    const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
    LIBSSH2_USERAUTH_KBDINT_RESPONSE*     responses,
    void**                                abstract)
{
    auto* ctx = static_cast<BastionKbdIntCtx*>(*abstract);

    term::transport::KbdIntChallenge challenge;
    challenge.name        = std::string(name,        static_cast<size_t>(name_len));
    challenge.instruction = std::string(instruction, static_cast<size_t>(instruction_len));
    for (int i = 0; i < num_prompts; ++i)
        challenge.prompts.push_back({
            std::string(reinterpret_cast<const char*>(prompts[i].text), prompts[i].length),
            prompts[i].echo != 0
        });

    const auto answers = ctx->target->OnKbdIntChallenge(challenge);

    for (int i = 0; i < num_prompts; ++i) {
        const std::string& ans = (i < static_cast<int>(answers.size())) ? answers[i] : "";
        responses[i].text   = strdup(ans.c_str());
        responses[i].length = static_cast<unsigned int>(ans.size());
    }
}

} // namespace

// ---------------------------------------------------------------------------
// BastionTunnel::Connect
// ---------------------------------------------------------------------------

std::unique_ptr<BastionTunnel> BastionTunnel::Connect(
    const term::session::ProxyJumpDesc& jump,
    const std::string&                  targetHost,
    unsigned short                      targetPort,
    const std::string&                  effectiveUser,
    int                                 connectTimeoutSec,
    ITransportTarget&                   target)
{
    using AM = term::session::SshAuthMethod;

    std::string err;

    // 1. TCP connect to jump host.
    const int fd = TcpConnect(jump.host, jump.port, connectTimeoutSec, err);
    if (fd < 0) throw TransportError{TransportError::Category::Connection, err};

    // 2. SSH handshake with jump host (blocking).
    LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session) {
        ::close(fd);
        throw TransportError{TransportError::Category::Connection, "SSH ProxyJump: libssh2_session_init failed"};
    }
    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, fd) != 0) {
        char* msg = nullptr; int len = 0;
        libssh2_session_last_error(session, &msg, &len, 0);
        std::string detail = msg ? std::string(msg, static_cast<size_t>(len)) : "unknown";
        libssh2_session_free(session);
        ::close(fd);
        throw TransportError{TransportError::Category::Connection, "SSH ProxyJump: handshake with " + jump.host + " failed — " + detail};
    }

    // 3. Host key verification.
    if (!VerifyBastionHostKey(session, jump.host, jump.port, err)) {
        libssh2_session_disconnect(session, "host key mismatch");
        libssh2_session_free(session);
        ::close(fd);
        throw TransportError{TransportError::Category::Connection, err};
    }

    // 4. Authenticate to jump host.
    bool authenticated = false;
    if (jump.authMethod == AM::Agent) {
        std::vector<std::vector<uint8_t>> preferred;
        if (!jump.agentIdentityHint.empty()) {
            std::filesystem::path p(jump.agentIdentityHint);
            if (p.extension() != ".pub") p += ".pub";
            auto blob = LoadPublicKeyBlob(p);
            if (!blob.empty()) preferred.push_back(std::move(blob));
        }
        authenticated = AgentAuthBlocking(session, effectiveUser.c_str(), preferred);
        if (!authenticated) err = "SSH ProxyJump: agent authentication to " + jump.host + " failed";

    } else if (jump.authMethod == AM::Password) {
        authenticated = libssh2_userauth_password(
            session, effectiveUser.c_str(), jump.password.c_str()) == 0;
        if (!authenticated) err = "SSH ProxyJump: password authentication to " + jump.host + " failed";

    } else if (jump.authMethod == AM::KbdInteractive) {
        BastionKbdIntCtx ctx{ &target };
        *libssh2_session_abstract(session) = &ctx;
        authenticated = libssh2_userauth_keyboard_interactive(
            session, effectiveUser.c_str(), &BastionKbdIntCallback) == 0;
        if (!authenticated) err = "SSH ProxyJump: keyboard-interactive authentication to " + jump.host + " failed";

    } else { // PrivateKey
        const char* pub = jump.publicKeyPath.empty()  ? nullptr : jump.publicKeyPath.c_str();
        const char* pp  = jump.passphrase.empty()     ? nullptr : jump.passphrase.c_str();
        authenticated = libssh2_userauth_publickey_fromfile(
            session, effectiveUser.c_str(),
            pub, jump.privateKeyPath.c_str(), pp) == 0;
        if (!authenticated) err = "SSH ProxyJump: private key authentication to " + jump.host + " failed";
    }

    if (!authenticated) {
        libssh2_session_disconnect(session, "authentication failed");
        libssh2_session_free(session);
        ::close(fd);
        throw TransportError{TransportError::Category::Connection, err};
    }

    // 5. Open direct-tcpip channel to the final target.
    LIBSSH2_CHANNEL* channel = nullptr;
    for (;;) {
        channel = libssh2_channel_direct_tcpip_ex(
            session, targetHost.c_str(), static_cast<int>(targetPort),
            "127.0.0.1", 0);
        if (channel) break;
        char* msg = nullptr; int len = 0;
        libssh2_session_last_error(session, &msg, &len, 0);
        std::string detail = msg ? std::string(msg, static_cast<size_t>(len)) : "unknown";
        libssh2_session_disconnect(session, "channel open failed");
        libssh2_session_free(session);
        ::close(fd);
        throw TransportError{TransportError::Category::Connection,
            "SSH ProxyJump: could not open tunnel from " + jump.host +
            " to " + targetHost + ":" + std::to_string(targetPort) +
            " — " + detail};
    }

    // 6. Create socketpair for bridge.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        libssh2_channel_free(channel);
        libssh2_session_disconnect(session, "socketpair failed");
        libssh2_session_free(session);
        ::close(fd);
        throw TransportError{TransportError::Category::Connection, "SSH ProxyJump: socketpair() failed"};
    }

    auto tun = std::unique_ptr<BastionTunnel>(new BastionTunnel());
    tun->bastion_session_ = session;
    tun->tunnel_channel_  = channel;
    tun->bastion_sock_    = fd;
    tun->bridge_fd_       = sv[0];
    tun->local_fd_        = sv[1];

    // 7. Switch to non-blocking mode for the bridge thread.
    libssh2_session_set_blocking(session, 0);

    // 8. Start bridge thread.
    tun->bridge_thread_ = std::thread(&BastionTunnel::BridgeLoop, tun.get());

    return tun;
}

// ---------------------------------------------------------------------------
// BastionTunnel::BridgeLoop
// ---------------------------------------------------------------------------

void BastionTunnel::BridgeLoop()
{
    while (!stop_.load(std::memory_order_relaxed)) {
        // Determine which events to watch on the bastion socket.
        const int dirs = libssh2_session_block_directions(bastion_session_);
        pollfd pfds[2];
        pfds[0] = { bastion_sock_, 0, 0 };
        if (dirs & LIBSSH2_SESSION_BLOCK_INBOUND)  pfds[0].events |= POLLIN;
        if (dirs & LIBSSH2_SESSION_BLOCK_OUTBOUND) pfds[0].events |= POLLOUT;
        if (!pfds[0].events) pfds[0].events = POLLIN;  // default: wait for inbound
        pfds[1] = { bridge_fd_, POLLIN, 0 };

        if (::poll(pfds, 2, kBridgePollMs) < 0) break;

        // Channel → bridge_fd (data arriving from the remote target via bastion).
        for (;;) {
            char buf[16384];
            const ssize_t n = libssh2_channel_read(tunnel_channel_, buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) break;
            if (n < 0) goto done;
            if (n == 0) {
                if (libssh2_channel_eof(tunnel_channel_)) goto done;
                break;
            }
            for (size_t off = 0; off < static_cast<size_t>(n);) {
                const ssize_t w = ::write(bridge_fd_, buf + off,
                                          static_cast<size_t>(n) - off);
                if (w <= 0) goto done;
                off += static_cast<size_t>(w);
            }
        }

        if (libssh2_channel_eof(tunnel_channel_)) goto done;

        // bridge_fd → channel (data from the target libssh2 session → bastion).
        if (pfds[1].revents & (POLLIN | POLLHUP)) {
            char buf[16384];
            const ssize_t n = ::read(bridge_fd_, buf, sizeof(buf));
            if (n <= 0) goto done;
            for (size_t off = 0; off < static_cast<size_t>(n);) {
                const ssize_t w = libssh2_channel_write(
                    tunnel_channel_, buf + off, static_cast<size_t>(n) - off);
                if (w == LIBSSH2_ERROR_EAGAIN) {
                    pollfd wp{ bastion_sock_, POLLOUT, 0 };
                    if (::poll(&wp, 1, kBridgePollMs) <= 0) goto done;
                    continue;
                }
                if (w < 0) goto done;
                off += static_cast<size_t>(w);
            }
        }
    }

done:
    // Signal the target session that the tunnel is gone by closing its fd.
    // The target libssh2 session will receive EOF and wind down.
    if (local_fd_ >= 0) {
        ::shutdown(local_fd_, SHUT_RDWR);
    }
}

// ---------------------------------------------------------------------------
// BastionTunnel::Stop / destructor
// ---------------------------------------------------------------------------

void BastionTunnel::Stop()
{
    stop_.store(true, std::memory_order_relaxed);

    // Closing bridge_fd_ unblocks any blocked read() in the bridge thread.
    if (bridge_fd_ >= 0) {
        ::close(bridge_fd_);
        bridge_fd_ = -1;
    }

    if (bridge_thread_.joinable())
        bridge_thread_.join();

    if (tunnel_channel_) {
        libssh2_channel_send_eof(tunnel_channel_);
        libssh2_channel_free(tunnel_channel_);
        tunnel_channel_ = nullptr;
    }
    if (bastion_session_) {
        libssh2_session_disconnect(bastion_session_, "tunnel closed");
        libssh2_session_free(bastion_session_);
        bastion_session_ = nullptr;
    }
    if (bastion_sock_ >= 0) {
        ::close(bastion_sock_);
        bastion_sock_ = -1;
    }
    if (local_fd_ >= 0) {
        ::close(local_fd_);
        local_fd_ = -1;
    }
}

BastionTunnel::~BastionTunnel()
{
    Stop();
}

} // namespace term::transport
