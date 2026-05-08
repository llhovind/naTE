#include "transport/SshTransport.h"

#include <libssh2.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace term::transport {

namespace {

constexpr char kTermType[]     = "xterm-256color";
constexpr int  kPollTimeoutMs  = 100;

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SshTransport::SshTransport(ITransportTarget& target,
                           const term::session::SshDesc& desc,
                           unsigned short cols,
                           unsigned short rows)
    : target_(target), desc_(desc), cols_(cols), rows_(rows)
{}

SshTransport::~SshTransport()
{
    Stop();

    // Zero secrets in memory after the worker has finished.
    std::fill(desc_.password.begin(),   desc_.password.end(),   '\0');
    std::fill(desc_.passphrase.begin(), desc_.passphrase.end(), '\0');
}

// ---------------------------------------------------------------------------
// Transport interface
// ---------------------------------------------------------------------------

void SshTransport::Start()
{
    running_ = true;
    worker_  = std::thread(&SshTransport::WorkerThread, this);
}

void SshTransport::Stop()
{
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

void SshTransport::Write(const std::string& data)
{
    std::lock_guard<std::mutex> lk(queue_mutex_);
    write_queue_.push_back(data);
}

void SshTransport::Resize(unsigned short cols, unsigned short rows)
{
    std::lock_guard<std::mutex> lk(queue_mutex_);
    pending_cols_   = cols;
    pending_rows_   = rows;
    resize_pending_ = true;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void SshTransport::WorkerThread()
{
    sock_fd_ = ConnectSocket();
    if (sock_fd_ < 0)                   return;
    if (!PerformHandshake(sock_fd_))    return;
    if (!VerifyHostKey())               return;
    if (!Authenticate())                return;
    if (!OpenChannel())                 return;
    if (!RequestPty())                  return;

    if (desc_.keepaliveSeconds > 0)
        libssh2_keepalive_config(session_, 1, static_cast<unsigned>(desc_.keepaliveSeconds));

    if (desc_.compress)
        libssh2_session_flag(session_, LIBSSH2_FLAG_COMPRESS, 1);

    ReadWriteLoop();

    // Orderly teardown (best-effort; ignore errors during shutdown).
    if (channel_) {
        libssh2_channel_close(channel_);
        libssh2_channel_free(channel_);
        channel_ = nullptr;
    }
    if (agent_) {
        libssh2_agent_disconnect(agent_);
        libssh2_agent_free(agent_);
        agent_ = nullptr;
    }
    if (session_) {
        libssh2_session_disconnect(session_, "Normal shutdown");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    target_.OnDisconnect();
}

// ---------------------------------------------------------------------------
// ConnectSocket
// ---------------------------------------------------------------------------

int SshTransport::ConnectSocket()
{
    const std::string portStr = std::to_string(desc_.port);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(desc_.host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0) {
        NotifyError(TransportError::Category::Connection,
                    "SSH: name resolution failed for '" + desc_.host +
                    "': " + gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        // Apply connect timeout via SO_SNDTIMEO.
        if (desc_.connectTimeoutSec > 0) {
            timeval tv{};
            tv.tv_sec = desc_.connectTimeoutSec;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                         &tv, sizeof(tv));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                         &tv, sizeof(tv));
        }

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;

        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);

    if (fd < 0) {
        NotifyError(TransportError::Category::Connection,
                    "SSH: could not connect to " + desc_.host + ":" + portStr +
                    " — " + std::strerror(errno));
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// PerformHandshake
// ---------------------------------------------------------------------------

bool SshTransport::PerformHandshake(int fd)
{
    session_ = libssh2_session_init();
    if (!session_) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: libssh2_session_init failed");
        return false;
    }

    libssh2_session_set_blocking(session_, 0);

    int rc;
    while ((rc = libssh2_session_handshake(session_, fd)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        if (!PollUntilReady(kPollTimeoutMs)) return false;
    }
    if (rc != 0) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: handshake failed — " + LastSshError());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// VerifyHostKey — silent TOFU
// ---------------------------------------------------------------------------

bool SshTransport::VerifyHostKey()
{
    size_t keyLen  = 0;
    int    keyType = 0;
    const char* key = libssh2_session_hostkey(session_, &keyLen, &keyType);
    if (!key) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: could not retrieve server host key");
        return false;
    }

    LIBSSH2_KNOWNHOSTS* hosts = libssh2_knownhost_init(session_);
    if (!hosts) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: libssh2_knownhost_init failed");
        return false;
    }

    const std::string khPath = KnownHostsPath();
    // Read existing entries; ignore error if the file doesn't exist yet.
    libssh2_knownhost_readfile(hosts, khPath.c_str(),
                               LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    int typeFlag = LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                   LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (keyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:     typeFlag |= LIBSSH2_KNOWNHOST_KEY_SSHRSA;   break;
        case LIBSSH2_HOSTKEY_TYPE_DSS:     typeFlag |= LIBSSH2_KNOWNHOST_KEY_SSHDSS;   break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ECDSA_256; break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ECDSA_384; break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ECDSA_521; break;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: typeFlag |= LIBSSH2_KNOWNHOST_KEY_ED25519;  break;
        default:                           typeFlag |= LIBSSH2_KNOWNHOST_KEY_UNKNOWN;   break;
    }

    libssh2_knownhost* found = nullptr;
    int check = libssh2_knownhost_checkp(
        hosts,
        desc_.host.c_str(), static_cast<int>(desc_.port),
        key, keyLen,
        typeFlag,
        &found);

    bool ok = false;
    switch (check) {
        case LIBSSH2_KNOWNHOST_CHECK_MATCH:
            ok = true;
            break;

        case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND:
            // TOFU: add and persist.
            libssh2_knownhost_addc(hosts,
                                   desc_.host.c_str(), nullptr,
                                   key, keyLen,
                                   nullptr, 0,
                                   typeFlag, nullptr);
            libssh2_knownhost_writefile(hosts, khPath.c_str(),
                                        LIBSSH2_KNOWNHOST_FILE_OPENSSH);
            ok = true;
            break;

        case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
            NotifyError(TransportError::Category::HostKey,
                        "SSH: HOST KEY MISMATCH for " + desc_.host +
                        " — possible man-in-the-middle attack.\n"
                        "Remove the entry from " + khPath + " to proceed.");
            ok = false;
            break;

        default:
            NotifyError(TransportError::Category::HostKey,
                        "SSH: host key check failed (code " +
                        std::to_string(check) + ")");
            ok = false;
            break;
    }

    libssh2_knownhost_free(hosts);
    return ok;
}

// ---------------------------------------------------------------------------
// Authenticate
// ---------------------------------------------------------------------------

bool SshTransport::Authenticate()
{
    using AM = term::session::SshAuthMethod;
    switch (desc_.authMethod) {
        case AM::Agent:      return AuthViaAgent();
        case AM::Password:   return AuthViaPassword();
        case AM::PrivateKey: return AuthViaPrivateKey();
    }
    return false;
}

bool SshTransport::AuthViaAgent()
{
    agent_ = libssh2_agent_init(session_);
    if (!agent_) {
        NotifyError(TransportError::Category::Authentication,
                    "SSH: could not initialise SSH agent");
        return false;
    }

    if (libssh2_agent_connect(agent_) != 0) {
        NotifyError(TransportError::Category::Authentication,
                    "SSH: could not connect to SSH agent — is SSH_AUTH_SOCK set?");
        return false;
    }

    if (libssh2_agent_list_identities(agent_) != 0) {
        NotifyError(TransportError::Category::Authentication,
                    "SSH: could not list SSH agent identities");
        return false;
    }

    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* prev     = nullptr;

    while (running_) {
        int rc = libssh2_agent_get_identity(agent_, &identity, prev);
        if (rc == 1) {
            // No more identities.
            NotifyError(TransportError::Category::Authentication,
                        "SSH: agent has no identity that was accepted by the server");
            return false;
        }
        if (rc < 0) {
            NotifyError(TransportError::Category::Authentication,
                        "SSH: agent identity enumeration failed");
            return false;
        }

        int auth;
        while ((auth = libssh2_agent_userauth(agent_, desc_.username.c_str(), identity))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return false;
            PollUntilReady(kPollTimeoutMs);
        }
        if (auth == 0)
            return true;

        prev = identity;
    }
    return false;
}

bool SshTransport::AuthViaPassword()
{
    int rc;
    while ((rc = libssh2_userauth_password(
                session_,
                desc_.username.c_str(),
                desc_.password.c_str())) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }
    if (rc != 0) {
        NotifyError(TransportError::Category::Authentication,
                    "SSH: password authentication failed — " + LastSshError());
        return false;
    }
    return true;
}

bool SshTransport::AuthViaPrivateKey()
{
    const char* pubkey = desc_.publicKeyPath.empty()
                         ? nullptr
                         : desc_.publicKeyPath.c_str();
    const char* passphrase = desc_.passphrase.empty()
                             ? nullptr
                             : desc_.passphrase.c_str();

    int rc;
    while ((rc = libssh2_userauth_publickey_fromfile(
                session_,
                desc_.username.c_str(),
                pubkey,
                desc_.privateKeyPath.c_str(),
                passphrase)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }
    if (rc != 0) {
        NotifyError(TransportError::Category::Authentication,
                    "SSH: private key authentication failed — " + LastSshError());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// OpenChannel / RequestPty
// ---------------------------------------------------------------------------

bool SshTransport::OpenChannel()
{
    while (running_) {
        channel_ = libssh2_channel_open_session(session_);
        if (channel_)
            return true;
        if (libssh2_session_last_error(session_, nullptr, nullptr, 0) !=
            LIBSSH2_ERROR_EAGAIN) {
            NotifyError(TransportError::Category::Protocol,
                        "SSH: could not open channel — " + LastSshError());
            return false;
        }
        if (!PollUntilReady(kPollTimeoutMs)) return false;
    }
    return false;
}

bool SshTransport::RequestPty()
{
    // Request a PTY.
    int rc;
    while ((rc = libssh2_channel_request_pty_ex(
                channel_,
                kTermType, static_cast<unsigned>(std::strlen(kTermType)),
                nullptr, 0,
                cols_, rows_,
                LIBSSH2_TERM_WIDTH_PX, LIBSSH2_TERM_HEIGHT_PX)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }
    if (rc != 0) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: PTY request failed — " + LastSshError());
        return false;
    }

    // Start the shell or a specific command.
    const bool hasCommand = !desc_.remoteCommand.empty();
    if (hasCommand) {
        while ((rc = libssh2_channel_exec(channel_, desc_.remoteCommand.c_str()))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return false;
            PollUntilReady(kPollTimeoutMs);
        }
    } else {
        while ((rc = libssh2_channel_shell(channel_)) == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return false;
            PollUntilReady(kPollTimeoutMs);
        }
    }

    if (rc != 0) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: could not start " +
                    std::string(hasCommand ? "command" : "shell") +
                    " — " + LastSshError());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ReadWriteLoop
// ---------------------------------------------------------------------------

void SshTransport::ReadWriteLoop()
{
    constexpr size_t kReadBuf = 4096;
    char buf[kReadBuf];

    int secondsToNext = desc_.keepaliveSeconds > 0 ? desc_.keepaliveSeconds : 0;

    while (running_) {
        // Determine poll timeout: shorter of kPollTimeoutMs and next keepalive.
        int pollMs = kPollTimeoutMs;
        if (secondsToNext > 0)
            pollMs = std::min(pollMs, secondsToNext * 1000);

        // Poll the socket for readability (or writability if libssh2 requests it).
        int dir = libssh2_session_block_directions(session_);
        pollfd pfd{};
        pfd.fd     = sock_fd_;
        pfd.events = ((dir & LIBSSH2_SESSION_BLOCK_INBOUND)  ? POLLIN  : 0) |
                     ((dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0);
        if (pfd.events == 0) pfd.events = POLLIN; // always at least wait for data
        ::poll(&pfd, 1, pollMs);

        if (!running_) break;

        // Read available data from the channel.
        ssize_t nRead;
        while ((nRead = libssh2_channel_read(channel_, buf, kReadBuf)) > 0)
            target_.OnData(std::string(buf, static_cast<size_t>(nRead)));

        if (nRead == 0 || libssh2_channel_eof(channel_)) {
            // Remote side closed the channel.
            break;
        }
        if (nRead != LIBSSH2_ERROR_EAGAIN && nRead < 0) {
            // Unexpected read error; exit the loop and let the teardown fire OnDisconnect.
            break;
        }

        // Drain pending writes and apply any pending resize.
        DrainWriteQueue();

        // Keep-alive.
        if (desc_.keepaliveSeconds > 0) {
            int next = 0;
            libssh2_keepalive_send(session_, &next);
            secondsToNext = next;
        }
    }
}

// ---------------------------------------------------------------------------
// DrainWriteQueue
// ---------------------------------------------------------------------------

void SshTransport::DrainWriteQueue()
{
    // Swap the resize flag and queue out under lock, then act without holding it.
    bool           doResize = false;
    unsigned short newCols  = 0;
    unsigned short newRows  = 0;
    std::deque<std::string> local;

    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        local.swap(write_queue_);
        if (resize_pending_) {
            doResize        = true;
            newCols         = pending_cols_;
            newRows         = pending_rows_;
            cols_           = newCols;
            rows_           = newRows;
            resize_pending_ = false;
        }
    }

    // Send pending writes.
    for (const auto& chunk : local) {
        size_t sent = 0;
        while (sent < chunk.size() && running_) {
            ssize_t n = libssh2_channel_write(
                channel_,
                chunk.data() + sent,
                chunk.size() - sent);
            if (n == LIBSSH2_ERROR_EAGAIN) {
                PollUntilReady(kPollTimeoutMs);
                continue;
            }
            if (n < 0) return;
            sent += static_cast<size_t>(n);
        }
    }

    // Apply resize.
    if (doResize && channel_) {
        int rc;
        while ((rc = libssh2_channel_request_pty_size(channel_, newCols, newRows))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return;
            PollUntilReady(kPollTimeoutMs);
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool SshTransport::PollUntilReady(int timeout_ms)
{
    int dir = libssh2_session_block_directions(session_);
    pollfd pfd{};
    pfd.fd     = sock_fd_;
    pfd.events = ((dir & LIBSSH2_SESSION_BLOCK_INBOUND)  ? POLLIN  : 0) |
                 ((dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0);
    if (pfd.events == 0) pfd.events = POLLIN;
    ::poll(&pfd, 1, timeout_ms);
    return running_.load();
}

void SshTransport::NotifyError(TransportError::Category category,
                               const std::string& msg)
{
    target_.OnData("\r\n\x1b[31m" + msg + "\x1b[0m\r\n");
    target_.OnError(TransportError{category, msg});
    target_.OnDisconnect();
    running_ = false;
}

std::string SshTransport::LastSshError() const
{
    if (!session_) return "(no session)";
    char* msg  = nullptr;
    int   len  = 0;
    libssh2_session_last_error(session_, &msg, &len, 0);
    return msg ? std::string(msg, static_cast<size_t>(len)) : "(unknown)";
}

std::string SshTransport::KnownHostsPath()
{
    const char* home = ::getenv("HOME");
    if (!home || home[0] == '\0') home = "/tmp";

    const std::string dir  = std::string(home) + "/.nate";
    const std::string path = dir + "/known_hosts";

    // Create ~/.nate with restricted permissions if it doesn't exist.
    ::mkdir(dir.c_str(), 0700);

    return path;
}

} // namespace term::transport
