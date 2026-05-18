#include "transport/SshTransport.h"
#include "transport/EnvUtils.h"

#include <libssh2.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
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

std::string GenerateVpColumnsFilePath() {
    static std::atomic<int> counter{0};
    return "/tmp/nate-vpcolumns-"
         + std::to_string(::getpid())
         + "-"
         + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SshTransport::SshTransport(ITransportTarget& target,
                           const term::session::SshDesc& desc,
                           unsigned short cols,
                           unsigned short rows,
                           unsigned short viewportCols,
                           const term::session::SessionInit& sessionInit,
                           const term::session::AppSessionDefaults& appDefaults)
    : target_(target), desc_(desc), sessionInit_(sessionInit),
      appDefaults_(appDefaults), cols_(cols), rows_(rows),
      viewportCols_(viewportCols),
      vpcolumns_remote_path_(GenerateVpColumnsFilePath())
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

void SshTransport::OnViewportColsChanged(unsigned short cols)
{
    std::lock_guard<std::mutex> lk(queue_mutex_);
    vpcolumns_pending_ = true;
    pending_vpcols_    = cols;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void SshTransport::WorkerThread()
{
    sock_fd_ = ConnectSocket();
    if (sock_fd_ < 0)                   return;
    if (!PerformHandshake(sock_fd_))    return;
    { std::string khErr;
      if (!VerifyHostKey(session_, khErr)) {
          NotifyError(TransportError::Category::HostKey, khErr);
          return;
      }
    }
    if (!Authenticate())                return;
    if (!OpenChannel())                 return;
    if (!RequestPty())                  return;

    if (desc_.keepaliveSeconds > 0)
        libssh2_keepalive_config(session_, 1, static_cast<unsigned>(desc_.keepaliveSeconds));

    if (desc_.compress)
        libssh2_session_flag(session_, LIBSSH2_FLAG_COMPRESS, 1);

    ReadWriteLoop();

    // Best-effort: remove the remote vpcolumns file before tearing down.
    // Switch to blocking mode — PollUntilReady returns false once running_ is
    // cleared, so the non-blocking EAGAIN retry loops don't work here.
    if (!vpcolumns_remote_path_.empty() && session_) {
        libssh2_session_set_blocking(session_, 1);
        LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(session_);
        if (ch) {
            const std::string rm = "rm -f '" + vpcolumns_remote_path_ + "'";
            if (libssh2_channel_exec(ch, rm.c_str()) == 0) {
                char discard[64];
                while (libssh2_channel_read(ch, discard, sizeof(discard)) > 0) {}
            }
            libssh2_channel_free(ch);
        }
        // Session remains in blocking mode — it's freed immediately after.
    }

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

bool SshTransport::VerifyHostKey(_LIBSSH2_SESSION* session, std::string& outError)
{
    size_t keyLen  = 0;
    int    keyType = 0;
    const char* key = libssh2_session_hostkey(session, &keyLen, &keyType);
    if (!key) {
        outError = "SSH: could not retrieve server host key";
        return false;
    }

    LIBSSH2_KNOWNHOSTS* hosts = libssh2_knownhost_init(session);
    if (!hosts) {
        outError = "SSH: libssh2_knownhost_init failed";
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
            outError = "SSH: HOST KEY MISMATCH for " + desc_.host +
                       " — possible man-in-the-middle attack.\n"
                       "Remove the entry from " + khPath + " to proceed.";
            ok = false;
            break;

        default:
            outError = "SSH: host key check failed (code " +
                       std::to_string(check) + ")";
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

    // Build and launch the effective remote command, injecting env vars and
    // working directory via the command string. libssh2_channel_setenv_ex() is
    // not used because AcceptEnv is disabled on most servers.
    {
        // Env file path is local — expand tilde against the local HOME.
        const char* localHomeRaw = getenv("HOME");
        const std::string localHome = localHomeRaw ? localHomeRaw : "";

        const std::string& rawEnvFile = sessionInit_.envFilePath.empty()
            ? appDefaults_.envFilePath
            : sessionInit_.envFilePath;
        const std::vector<term::session::EnvVar> fileVars =
            ParseEnvFile(ExpandTilde(rawEnvFile, localHome));

        // Merge: app defaults → file vars → profile vars (profile wins)
        std::vector<term::session::EnvVar> merged = appDefaults_.envVars;
        for (const auto& ev : fileVars)              merged.push_back(ev);
        for (const auto& ev : sessionInit_.envVars)  merged.push_back(ev);

        // Build a POSIX-safe export prefix: single-quote each value,
        // escaping any embedded single quotes as '\''.
        std::string envPrefix;
        for (const auto& ev : merged) {
            if (ev.key.empty()) continue;
            std::string escaped = ev.value;
            for (size_t pos = 0; (pos = escaped.find('\'', pos)) != std::string::npos; pos += 4)
                escaped.replace(pos, 1, "'\\''");
            envPrefix += "export " + ev.key + "='" + escaped + "'; ";
        }

        // Inject NATE_VPCOLUMNS_FILE and write the initial viewport width so
        // the shell can read the actual display columns even when COLUMNS is
        // inflated by wrap-OFF mode.
        {
            std::string escaped = vpcolumns_remote_path_;
            for (size_t p = 0; (p = escaped.find('\'', p)) != std::string::npos; p += 4)
                escaped.replace(p, 1, "'\\''");
            envPrefix += "export NATE_VPCOLUMNS_FILE='" + escaped + "'; ";
            envPrefix += "printf '%d\\n' " + std::to_string(viewportCols_)
                       + " > '" + escaped + "'; ";
        }

        // Resolve remote working directory. Do NOT expand ~ locally — the path
        // lives on the remote machine. Replace a leading ~ with $HOME so the
        // remote shell expands it correctly inside double quotes.
        const std::string& rawDir = sessionInit_.workingDir.empty()
            ? appDefaults_.workingDir
            : sessionInit_.workingDir;
        std::string sshDir = rawDir;
        if (!sshDir.empty() && sshDir[0] == '~')
            sshDir = "$HOME" + sshDir.substr(1);

        // Assemble: [exports] [cd "dir" &&] [remoteCommand | exec $SHELL]
        std::string effectiveCmd = desc_.remoteCommand;
        if (!sshDir.empty()) {
            effectiveCmd = envPrefix + "cd \"" + sshDir + "\" && "
                         + (effectiveCmd.empty() ? "exec $SHELL" : effectiveCmd);
        } else if (!envPrefix.empty()) {
            effectiveCmd = envPrefix + (effectiveCmd.empty() ? "exec $SHELL" : effectiveCmd);
        }
        // If both are empty, effectiveCmd remains "" → channel_shell() below.

        // Start the shell or effective command.
        const bool hasCommand = !effectiveCmd.empty();
        if (hasCommand) {
            while ((rc = libssh2_channel_exec(channel_, effectiveCmd.c_str()))
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
    bool           doResize  = false;
    unsigned short newCols   = 0;
    unsigned short newRows   = 0;
    bool           doVpCols  = false;
    unsigned short newVpCols = 0;
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
        if (vpcolumns_pending_) {
            doVpCols           = true;
            newVpCols          = pending_vpcols_;
            vpcolumns_pending_ = false;
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

    // Update the remote vpcolumns file if the viewport width changed.
    if (doVpCols && session_ && !vpcolumns_remote_path_.empty())
        RemoteWriteVpCols(newVpCols);
}

void SshTransport::RemoteWriteVpCols(unsigned short cols)
{
    const std::string cmd = "printf '%d\\n' " + std::to_string(cols)
                          + " > '" + vpcolumns_remote_path_ + "'";

    LIBSSH2_CHANNEL* ch = nullptr;
    while (running_) {
        ch = libssh2_channel_open_session(session_);
        if (ch) break;
        if (libssh2_session_last_error(session_, nullptr, nullptr, 0) != LIBSSH2_ERROR_EAGAIN)
            return;
        if (!PollUntilReady(kPollTimeoutMs)) return;
    }
    if (!ch) return;

    int rc;
    while ((rc = libssh2_channel_exec(ch, cmd.c_str())) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) break;
        PollUntilReady(kPollTimeoutMs);
    }

    libssh2_channel_send_eof(ch);
    // Drain output so the session multiplexer stays clean.
    char discard[64];
    while (libssh2_channel_read(ch, discard, sizeof(discard)) > 0) {}
    libssh2_channel_free(ch);
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

// ---------------------------------------------------------------------------
// File transfer — public interface
// ---------------------------------------------------------------------------

std::string SshTransport::GetRemoteDescription() const
{
    return desc_.username + "@" + desc_.host;
}

void SshTransport::TransferFile(const std::string& localPath,
                                const std::string& remoteDir,
                                std::function<void(bool, std::string)> onDone)
{
    std::thread(&SshTransport::DoTransferFile, this,
                localPath, remoteDir, std::move(onDone)).detach();
}

// ---------------------------------------------------------------------------
// DoTransferFile — runs on a detached thread, uses a fresh blocking session
// ---------------------------------------------------------------------------

void SshTransport::DoTransferFile(std::string localPath,
                                  std::string remoteDir,
                                  std::function<void(bool, std::string)> onDone)
{
    const auto fail = [&onDone](std::string msg) {
        onDone(false, std::move(msg));
    };

    // --- Open local file and get size ---
    std::ifstream file(localPath, std::ios::binary | std::ios::ate);
    if (!file) {
        fail("Cannot open local file: " + localPath);
        return;
    }
    const auto fileSize = static_cast<libssh2_uint64_t>(file.tellg());
    file.seekg(0);

    // Extract the base filename for the remote path.
    std::string filename = localPath;
    if (const auto pos = filename.rfind('/'); pos != std::string::npos)
        filename = filename.substr(pos + 1);

    // Ensure remoteDir ends with '/'.
    if (!remoteDir.empty() && remoteDir.back() != '/')
        remoteDir += '/';
    const std::string remotePath = remoteDir + filename;

    // --- TCP connect ---
    const std::string portStr = std::to_string(desc_.port);
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(desc_.host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        fail("SCP: name resolution failed for " + desc_.host);
        return;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (desc_.connectTimeoutSec > 0) {
            timeval tv{};
            tv.tv_sec = desc_.connectTimeoutSec;
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        fail("SCP: could not connect to " + desc_.host + ":" + portStr);
        return;
    }

    // --- libssh2 session (blocking mode) ---
    LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session) {
        ::close(fd);
        fail("SCP: libssh2_session_init failed");
        return;
    }
    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, fd) != 0) {
        char* msg = nullptr; int len = 0;
        libssh2_session_last_error(session, &msg, &len, 0);
        const std::string err = msg ? std::string(msg, static_cast<size_t>(len)) : "unknown";
        libssh2_session_free(session);
        ::close(fd);
        fail("SCP: handshake failed — " + err);
        return;
    }

    // --- Host key verification (reuse existing TOFU logic) ---
    {
        std::string khErr;
        if (!VerifyHostKey(session, khErr)) {
            libssh2_session_disconnect(session, "Host key check failed");
            libssh2_session_free(session);
            ::close(fd);
            fail(khErr);
            return;
        }
    }

    // --- Authentication (blocking — no EAGAIN loops needed) ---
    using AM = term::session::SshAuthMethod;
    bool authenticated = false;
    std::string authError;

    if (desc_.authMethod == AM::Agent) {
        LIBSSH2_AGENT* agent = libssh2_agent_init(session);
        if (agent && libssh2_agent_connect(agent) == 0 &&
            libssh2_agent_list_identities(agent) == 0) {
            libssh2_agent_publickey* id = nullptr;
            libssh2_agent_publickey* prev = nullptr;
            while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
                if (libssh2_agent_userauth(agent, desc_.username.c_str(), id) == 0) {
                    authenticated = true;
                    break;
                }
                prev = id;
            }
        }
        if (agent) {
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
        if (!authenticated)
            authError = "SCP: agent authentication failed";

    } else if (desc_.authMethod == AM::Password) {
        authenticated = libssh2_userauth_password(
            session, desc_.username.c_str(), desc_.password.c_str()) == 0;
        if (!authenticated)
            authError = "SCP: password authentication failed";

    } else { // PrivateKey
        const char* pubkey = desc_.publicKeyPath.empty()
                             ? nullptr : desc_.publicKeyPath.c_str();
        const char* passphrase = desc_.passphrase.empty()
                                 ? nullptr : desc_.passphrase.c_str();
        authenticated = libssh2_userauth_publickey_fromfile(
            session, desc_.username.c_str(),
            pubkey, desc_.privateKeyPath.c_str(), passphrase) == 0;
        if (!authenticated)
            authError = "SCP: private key authentication failed";
    }

    if (!authenticated) {
        libssh2_session_disconnect(session, "Authentication failed");
        libssh2_session_free(session);
        ::close(fd);
        fail(authError);
        return;
    }

    // --- SCP send ---
    LIBSSH2_CHANNEL* channel = libssh2_scp_send64(
        session, remotePath.c_str(), 0644, fileSize, 0, 0);
    if (!channel) {
        char* msg = nullptr; int len = 0;
        libssh2_session_last_error(session, &msg, &len, 0);
        const std::string err = msg ? std::string(msg, static_cast<size_t>(len)) : "unknown";
        libssh2_session_disconnect(session, "SCP open failed");
        libssh2_session_free(session);
        ::close(fd);
        fail("SCP: could not open remote file '" + remotePath + "' — " + err);
        return;
    }

    constexpr size_t kChunk = 32768;
    std::vector<char> buf(kChunk);
    bool writeError = false;
    while (file) {
        file.read(buf.data(), static_cast<std::streamsize>(kChunk));
        const std::streamsize nRead = file.gcount();
        if (nRead == 0) break;
        size_t sent = 0;
        while (sent < static_cast<size_t>(nRead)) {
            ssize_t n = libssh2_channel_write(
                channel, buf.data() + sent,
                static_cast<size_t>(nRead) - sent);
            if (n < 0) { writeError = true; break; }
            sent += static_cast<size_t>(n);
        }
        if (writeError) break;
    }

    libssh2_channel_send_eof(channel);
    libssh2_channel_wait_eof(channel);
    libssh2_channel_wait_closed(channel);
    libssh2_channel_free(channel);
    libssh2_session_disconnect(session, "SCP complete");
    libssh2_session_free(session);
    ::close(fd);

    if (writeError) {
        fail("SCP: write error while sending '" + filename + "'");
        return;
    }

    onDone(true, {});
}

} // namespace term::transport
