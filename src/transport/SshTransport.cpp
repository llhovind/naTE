#include "transport/SshTransport.h"
#include "transport/EnvUtils.h"
#include "transport/SshSession.h"
#include "transport/X11Utils.h"

#include <libssh2.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace term::transport {

namespace {

using term::transport::ssh::kPollTimeoutMs;

constexpr char kTermType[]            = "xterm-256color";
constexpr int  kCwdCaptureIntervalSec = 120;

// TCP keepalive parameters applied to every established SSH socket.
// The kernel sends the first probe after kTcpKeepIdleSec of silence, then
// retries every kTcpKeepIntvlSec up to kTcpKeepCnt times before declaring the
// connection dead.  Total worst-case detection time:
//   kTcpKeepIdleSec + kTcpKeepCnt * kTcpKeepIntvlSec = 10 + 3*10 = 40 s.
constexpr int  kTcpKeepIdleSec  = 10;
constexpr int  kTcpKeepIntvlSec = 10;
constexpr int  kTcpKeepCnt      = 3;

// libssh2 X11 channel-open callback — invoked on the worker thread from within
// libssh2_channel_read() when the server opens a reverse X11 channel.
// abstract is the session's user pointer, which WorkerThread sets to `this`.
void X11OpenCallback(LIBSSH2_SESSION* /*session*/,
                     LIBSSH2_CHANNEL* channel,
                     char* /*shost*/, int /*sport*/,
                     void** abstract)
{
    auto* self = static_cast<term::transport::SshTransport*>(*abstract);
    self->AcceptX11Channel(channel);
}

#if LIBSSH2_VERSION_NUM >= 0x010B00
// libssh2 auth-agent channel-open callback — invoked on the worker thread when
// the server opens an auth-agent@openssh.com reverse channel (libssh2 >= 1.11.0).
// abstract is the same session user pointer set to `this` in WorkerThread.
void AgentOpenCallback(LIBSSH2_SESSION* /*session*/,
                       LIBSSH2_CHANNEL* channel,
                       void** abstract)
{
    auto* self = static_cast<term::transport::SshTransport*>(*abstract);
    self->AcceptAgentChannel(channel);
}
#endif

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
                           const term::transport::SshDesc& desc,
                           unsigned short cols,
                           unsigned short rows,
                           unsigned short viewportCols,
                           const term::transport::SessionInit& sessionInit,
                           const term::transport::AppSessionDefaults& appDefaults)
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

void SshTransport::RequestX11Forwarding()
{
    x11_request_pending_.store(true);
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
    if (desc_.proxyJump) {
        const std::string effectiveUser = desc_.proxyJump->user.empty()
            ? desc_.username : desc_.proxyJump->user;
        try {
            bastion_tunnel_ = BastionTunnel::Connect(
                *desc_.proxyJump, desc_.host, desc_.port,
                effectiveUser, desc_.connectTimeoutSec, target_);
        } catch (const TransportError& e) {
            NotifyError(e.category, e.message);
            return;
        }
        sock_fd_ = bastion_tunnel_->LocalFd();
    } else {
        sock_fd_ = ConnectSocket();
        if (sock_fd_ < 0) return;
    }
    if (!PerformHandshake(sock_fd_))    return;
    { std::string khErr;
      if (!VerifyHostKey(khErr)) {
          NotifyError(TransportError::Category::HostKey, khErr);
          return;
      }
    }
    // Authenticate returns a typed result; the authenticator points the session
    // abstract at itself only for keyboard-interactive prompts, so we (re)claim
    // the slot here for the X11/agent channel-open callbacks registered below.
    if (const auto r = authenticator_.Authenticate(); !r.ok) {
        // Empty message => the transport is shutting down (running_ already
        // false); skip the user-facing error in that case, matching the prior
        // behaviour where a stop-during-auth returned without NotifyError.
        if (!r.message.empty())
            NotifyError(r.category, r.message);
        return;
    }
    *libssh2_session_abstract(session_) = this;

    // Register X11 callback — abstract set above.
#if LIBSSH2_VERSION_NUM >= 0x010B01
    libssh2_session_callback_set2(session_, LIBSSH2_CALLBACK_X11,
                                  reinterpret_cast<libssh2_cb_generic*>(X11OpenCallback));
#else
    libssh2_session_callback_set(session_, LIBSSH2_CALLBACK_X11,
                                 reinterpret_cast<void*>(X11OpenCallback));
#endif
    if (!OpenChannel())    return;
    if (!RequestPty())     return;
    // X11 forwarding request MUST precede shell start; the server sets $DISPLAY
    // in the shell's environment only if x11-req arrives before SSH_MSG_CHANNEL_REQUEST
    // for "shell"/"exec".
    if (desc_.x11Forwarding)   SetupX11Forwarding();
    if (desc_.agentForwarding) SetupAgentForwarding();
    if (!StartShell())         return;

    // Read the remote shell's PID from the first stdout line emitted by the
    // startup command ("printf '%d\n' $$; ...").  Any bytes read past the
    // newline are stashed in pidOvershoot_ and fed to the parser at the start
    // of ReadWriteLoop so no terminal output is lost.
    {
        std::string acc;
        while (running_ && acc.find('\n') == std::string::npos) {
            char tmp[256];
            const ssize_t n = libssh2_channel_read(channel_, tmp, sizeof(tmp));
            if (n > 0)
                acc.append(tmp, static_cast<size_t>(n));
            else if (n == LIBSSH2_ERROR_EAGAIN)
                PollUntilReady(kPollTimeoutMs);
            else
                break;
        }
        const auto nl = acc.find('\n');
        if (nl != std::string::npos) {
            try { remotePid_ = std::stoi(acc.substr(0, nl)); } catch (...) {}
            if (nl + 1 < acc.size())
                pidOvershoot_ = acc.substr(nl + 1);
        }
    }

    // SSH application-level keepalive is intentionally disabled.  SO_KEEPALIVE
    // (set in ConnectSocket) handles both dead-connection detection (~40 s) and
    // NAT state maintenance at the kernel level.  The libssh2 keepalive would
    // send SSH_MSG_GLOBAL_REQUEST periodically, which causes TCP to switch from
    // keepalive-probe mode into retransmit mode and prevents ETIMEDOUT from
    // being raised within the expected window.

    const auto disconnectReason = ReadWriteLoop();

    // Signal cancellation so pending SFTP tasks see !running_ and self-cancel.
    running_.store(false);
    sftpService_.CancelPending();

    // For Interrupted (dead socket), SO_ERROR was consumed by the recv() that
    // detected the failure.  Subsequent poll() calls on the socket may no longer
    // return POLLERR, causing any blocking libssh2 operation to wait indefinitely
    // for a server response that will never come.  Skip all network-sending
    // teardown steps; just free the in-memory resources and close the fd.
    const bool socketDead = (disconnectReason == DisconnectReason::Interrupted);

    // Best-effort teardown operations while the socket is still alive.
    if (!socketDead && session_) {
        libssh2_session_set_blocking(session_, 1);

        sftpService_.Shutdown();

        if (!vpcolumns_remote_path_.empty()) {
            LIBSSH2_CHANNEL* ch = libssh2_channel_open_session(session_);
            if (ch) {
                const std::string rm = "rm -f " + ShellQuote(vpcolumns_remote_path_);
                if (libssh2_channel_exec(ch, rm.c_str()) == 0) {
                    char discard[64];
                    while (libssh2_channel_read(ch, discard, sizeof(discard)) > 0) {}
                }
                libssh2_channel_free(ch);
            }
        }

    }

    // Orderly teardown: send close/disconnect messages only when the socket is
    // alive.  On a dead socket these sends would block in libssh2's internal
    // poll() loop waiting for a response the server can never deliver.
    // (Shutdown is idempotent — no-op if the orderly path above already ran.)
    sftpService_.Shutdown();
    if (channel_) {
        if (!socketDead)
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
        if (!socketDead)
            libssh2_session_disconnect(session_, "Normal shutdown");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    target_.OnDisconnect(disconnectReason);
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

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            // Clear the connect-phase send/receive timeouts so they don't
            // interfere with the long-lived ReadWriteLoop.
            const timeval tvZero{};
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tvZero, sizeof(tvZero));
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tvZero, sizeof(tvZero));

            // Enable TCP keepalive so the kernel detects a silently-dropped
            // connection (e.g. firewall rule added after session established)
            // in ~40 s rather than the default ~15 min TCP retransmit window.
            const int yes = 1;
            ::setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &yes,              sizeof(yes));
            ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &kTcpKeepIdleSec,  sizeof(kTcpKeepIdleSec));
            ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &kTcpKeepIntvlSec, sizeof(kTcpKeepIntvlSec));
            ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &kTcpKeepCnt,      sizeof(kTcpKeepCnt));
            break;
        }

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

    // Compression is negotiated during the handshake (key exchange), so the
    // flag must be set before libssh2_session_handshake() — setting it later
    // has no effect.
    if (desc_.compress)
        libssh2_session_flag(session_, LIBSSH2_FLAG_COMPRESS, 1);

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

bool SshTransport::VerifyHostKey(std::string& outError)
{
    _LIBSSH2_SESSION* session = session_;
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
    return true;
}

// SSH protocol requires x11-req to be sent BEFORE starting the shell so the
// server can set $DISPLAY in the shell's environment.  Returns true on success;
// on failure logs to the terminal and returns false (session continues without X11).
bool SshTransport::SetupX11Forwarding()
{
    const char* disp = ::getenv("DISPLAY");
    const auto [authName, authCookie] = term::transport::ReadXauthorityData(disp);
    const char* authNamePtr   = authName.empty()   ? nullptr : authName.c_str();
    const char* authCookiePtr = authCookie.empty() ? nullptr : authCookie.c_str();

    int rc;
    while ((rc = libssh2_channel_x11_req_ex(
                    channel_, 0, authNamePtr, authCookiePtr, 0))
           == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }
    if (rc != 0) {
        target_.OnData("\r\n\x1b[33mX11 forwarding: server denied the request "
                       "(check X11Forwarding in sshd_config).\x1b[0m\r\n");
        return false;
    }
    x11_active_ = true;
    target_.OnX11StateChanged(true);
    return true;
}

// Sends auth-agent-req@openssh.com on the channel so sshd sets SSH_AUTH_SOCK in
// the remote shell, then registers AgentOpenCallback so inbound agent channels are
// proxied to the local SSH agent.  Requires libssh2 >= 1.11.0 (1.10.x silently
// drops inbound auth-agent@openssh.com SSH_MSG_CHANNEL_OPEN, causing deadlocks).
bool SshTransport::SetupAgentForwarding()
{
#if LIBSSH2_VERSION_NUM >= 0x010B00
#if LIBSSH2_VERSION_NUM >= 0x010B01
    libssh2_session_callback_set2(session_, LIBSSH2_CALLBACK_AUTHAGENT,
                                  reinterpret_cast<libssh2_cb_generic*>(AgentOpenCallback));
#else
    libssh2_session_callback_set(session_, LIBSSH2_CALLBACK_AUTHAGENT,
                                 reinterpret_cast<void*>(AgentOpenCallback));
#endif

    int rc;
    while ((rc = libssh2_channel_request_auth_agent(channel_)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }
    if (rc != 0) {
        target_.OnData("\r\n\x1b[33mSSH agent forwarding: server denied the request "
                       "(check AllowAgentForwarding in sshd_config).\x1b[0m\r\n");
        return false;
    }
    return true;
#else
    target_.OnData("\r\n\x1b[33mSSH agent forwarding: not supported by the installed "
                   "libssh2 " LIBSSH2_VERSION " (inbound auth-agent channels are "
                   "silently dropped, causing remote agent queries to hang). "
                   "Upgrade to libssh2 >= 1.11.0 for full support.\x1b[0m\r\n");
    return false;
#endif
}

// Builds and launches the effective remote command (env vars + working dir +
// shell or remoteCommand).  Must be called after RequestPty() and, if X11
// forwarding is wanted, after SetupX11Forwarding().
bool SshTransport::StartShell()
{
    // Build and launch the effective remote command, injecting env vars and
    // working directory via the command string. libssh2_channel_setenv_ex() is
    // not used because AcceptEnv is disabled on most servers.

    // Env file path is local — expand tilde against the local HOME.
    const char* localHomeRaw = getenv("HOME");
    const std::string localHome = localHomeRaw ? localHomeRaw : "";

    const std::string& rawEnvFile = sessionInit_.envFilePath.empty()
        ? appDefaults_.envFilePath
        : sessionInit_.envFilePath;
    const std::vector<term::transport::EnvVar> fileVars =
        ParseEnvFile(ExpandTilde(rawEnvFile, localHome));

    // Merge: app defaults → file vars → profile vars (profile wins)
    std::vector<term::transport::EnvVar> merged = appDefaults_.envVars;
    for (const auto& ev : fileVars)              merged.push_back(ev);
    for (const auto& ev : sessionInit_.envVars)  merged.push_back(ev);

    // Build a POSIX-safe export prefix; ShellQuote handles embedded quotes.
    std::string envPrefix;
    for (const auto& ev : merged) {
        if (ev.key.empty()) continue;
        envPrefix += "export " + ev.key + "=" + ShellQuote(ev.value) + "; ";
    }

    // Inject NATE_VPCOLUMNS_FILE and write the initial viewport width so
    // the shell can read the actual display columns even when COLUMNS is
    // inflated by wrap-OFF mode.
    {
        const std::string quotedPath = ShellQuote(vpcolumns_remote_path_);
        envPrefix += "export NATE_VPCOLUMNS_FILE=" + quotedPath + "; ";
        envPrefix += "printf '%d\\n' " + std::to_string(viewportCols_)
                   + " > " + quotedPath + "; ";
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

    const bool useLogin = sessionInit_.loginShell || appDefaults_.loginShell;
    const std::string shellExec = useLogin ? "exec $SHELL -l" : "exec $SHELL";

    // Assemble: [exports] [cd "dir" &&] [remoteCommand | exec [-l] $SHELL]
    // Always use channel_exec so loginShell is applied consistently regardless
    // of whether env vars or a working directory are configured.
    std::string effectiveCmd = desc_.remoteCommand.empty() ? shellExec : desc_.remoteCommand;
    if (!sshDir.empty())
        effectiveCmd = envPrefix + "cd \"" + sshDir + "\" && " + effectiveCmd;
    else if (!envPrefix.empty())
        effectiveCmd = envPrefix + effectiveCmd;

    // Prepend a PID write as the very first action.  $$ is the PID of the
    // process executing this command string; exec $SHELL preserves it, so
    // remotePid_ stays valid for the lifetime of the session.
    effectiveCmd = "printf '%d\\n' $$; " + effectiveCmd;

    int rc;
    while ((rc = libssh2_channel_exec(channel_, effectiveCmd.c_str()))
           == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }

    if (rc != 0) {
        NotifyError(TransportError::Category::Protocol,
                    "SSH: could not start remote command — " + LastSshError());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ReadWriteLoop
// ---------------------------------------------------------------------------

DisconnectReason SshTransport::ReadWriteLoop()
{
    constexpr size_t kReadBuf = 4096;
    // Bound the per-iteration read burst so a saturating remote (e.g. `find /`)
    // cannot monopolise the loop in the inner read-while and starve
    // DrainWriteQueue() — that starvation is what delays an interactive Ctrl+C
    // from reaching the remote.  When the cap is hit we re-poll with a zero
    // timeout so any still-pending data is read immediately rather than waiting
    // kPollTimeoutMs, keeping throughput high while servicing writes between bursts.
    constexpr size_t kMaxReadBurst = 64 * 1024;
    char buf[kReadBuf];

    DisconnectReason reason = DisconnectReason::Deliberate;
    bool readBurstCapped = false;

    // Feed any bytes read past the PID newline during startup into the parser.
    if (!pidOvershoot_.empty()) {
        target_.OnData(pidOvershoot_);
        pidOvershoot_.clear();
    }

    while (running_) {
        // --- Service pending mid-session X11 forwarding request -----------
        // Note: OpenSSH sshd requires x11-req before the shell starts.
        // Mid-session requests (after shell is running) will be denied.
        // The reliable path is desc_.x11Forwarding = true, handled in WorkerThread
        // before StartShell().  We keep the pending flag so the UI gets feedback.
        if (x11_request_pending_.exchange(false) && !x11_active_) {
            target_.OnData("\r\n\x1b[33mX11 forwarding: cannot be enabled after the "
                           "session has started. Enable \"Forward X11\" in the "
                           "connection profile and reconnect.\x1b[0m\r\n");
        }

        // --- Build pollfd array -------------------------------------------
        // [0] = SSH socket; [1..N] = X11 local FDs; [N+1..M] = agent local FDs.
        {
            const int dir = libssh2_session_block_directions(session_);
            const short sshEvents = static_cast<short>(
                ((dir & LIBSSH2_SESSION_BLOCK_INBOUND)  ? POLLIN  : 0) |
                ((dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0));

            std::vector<pollfd> pfds;
            pfds.reserve(1 + x11_channels_.size() + agent_channels_.size());
            pfds.push_back({sock_fd_, sshEvents ? sshEvents : static_cast<short>(POLLIN), 0});
            for (const auto& x : x11_channels_)
                pfds.push_back({x.local_fd, POLLIN, 0});
            for (const auto& a : agent_channels_)
                pfds.push_back({a.local_fd, POLLIN, 0});

            // Append port forward listen fds and proxy conn local fds.
            pfwEngine_.AppendPollFds(pfds);

            // A capped read burst left data pending: skip the wait so the next
            // read picks it up immediately, otherwise block up to kPollTimeoutMs.
            const int pollTimeout = readBurstCapped ? 0 : kPollTimeoutMs;
            readBurstCapped = false;
            ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), pollTimeout);

            if (!running_) break;

            // Service local X11 / agent sockets → their SSH channels.
            for (size_t i = 0; i < x11_channels_.size(); ++i)
                x11_channels_[i].PumpLocalToChannel(pfds[1 + i].revents, buf, kReadBuf);

            const size_t agentBase = 1 + x11_channels_.size();
            for (size_t i = 0; i < agent_channels_.size(); ++i)
                agent_channels_[i].PumpLocalToChannel(pfds[agentBase + i].revents, buf, kReadBuf);

            // Service port forward listen fds and proxy connections (both directions).
            pfwEngine_.ServiceConns(pfds, buf, kReadBuf);
        }

        // --- Read from main shell channel ---------------------------------
        ssize_t nRead;
        size_t  burst = 0;
        while ((nRead = libssh2_channel_read(channel_, buf, kReadBuf)) > 0) {
            target_.OnData(std::string(buf, static_cast<size_t>(nRead)));
            burst += static_cast<size_t>(nRead);
            if (burst >= kMaxReadBurst) { readBurstCapped = true; break; }
        }

        // Only treat EOF/error when the burst drained naturally; a capped burst
        // ended on a successful read (nRead > 0), so there is nothing to report.
        if (!readBurstCapped) {
            if (nRead == 0 || libssh2_channel_eof(channel_)) {
                reason = DisconnectReason::Clean;
                break;
            }
            if (nRead != LIBSSH2_ERROR_EAGAIN && nRead < 0) {
                reason = DisconnectReason::Interrupted;
                break;
            }
        }

        // --- Service SSH X11 / agent channels → local sockets -------------
        // Channel data travels over sock_fd_, serviced after the main read.
        PumpChannelsToLocal(x11_channels_, buf, kReadBuf);
        SweepClosedProxies(x11_channels_);

        PumpChannelsToLocal(agent_channels_, buf, kReadBuf);
        SweepClosedProxies(agent_channels_);

        // --- Drain writes + resize + vpcolumns + port forwards -----------
        DrainWriteQueue();
        CaptureCwdPeriodic();
        sftpService_.Service();
        pfwEngine_.ServiceQueue();

    }

    ReleaseAllProxies(x11_channels_);
    ReleaseAllProxies(agent_channels_);
    pfwEngine_.Teardown();

    return reason;
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
                          + " > " + ShellQuote(vpcolumns_remote_path_);

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

std::string SshTransport::RemoteExecRead(const std::string& cmd)
{
    LIBSSH2_CHANNEL* ch = nullptr;
    while (running_) {
        ch = libssh2_channel_open_session(session_);
        if (ch) break;
        if (libssh2_session_last_error(session_, nullptr, nullptr, 0) != LIBSSH2_ERROR_EAGAIN)
            return {};
        if (!PollUntilReady(kPollTimeoutMs)) return {};
    }
    if (!ch) return {};

    int rc;
    while ((rc = libssh2_channel_exec(ch, cmd.c_str())) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) { libssh2_channel_free(ch); return {}; }
        PollUntilReady(kPollTimeoutMs);
    }

    std::string output;
    if (rc == 0) {
        char buf[4096];
        ssize_t n;
        while (running_) {
            n = libssh2_channel_read(ch, buf, sizeof(buf));
            if (n > 0)
                output.append(buf, static_cast<size_t>(n));
            else if (n == LIBSSH2_ERROR_EAGAIN)
                PollUntilReady(kPollTimeoutMs);
            else
                break;
        }
        if (!output.empty() && output.back() == '\n')
            output.pop_back();
    }

    libssh2_channel_free(ch);
    return output;
}

void SshTransport::CaptureCwdPeriodic()
{
    using namespace std::chrono;
    if (duration_cast<seconds>(steady_clock::now() - lastCwdCapture_).count()
            < kCwdCaptureIntervalSec)
        return;

    // Update unconditionally so a transient failure doesn't cause a tight
    // retry loop on every subsequent poll iteration.
    lastCwdCapture_ = steady_clock::now();

    if (remotePid_ <= 0 || !procFsAvailable_)
        return;

    const std::string cwd = RemoteExecRead(
        "readlink /proc/" + std::to_string(remotePid_) + "/cwd");
    if (cwd.empty()) {
        procFsAvailable_ = false;  // /proc not available on this server
        return;
    }
    target_.OnCwdChanged(cwd);
}

// ---------------------------------------------------------------------------
// X11 forwarding helpers
// ---------------------------------------------------------------------------

// Called from the static X11OpenCallback on the worker thread when the server
// opens a new X11 channel.  Connects a local socket to $DISPLAY and records
// the pair for bidirectional proxying in the read/write loop.
void SshTransport::AcceptX11Channel(LIBSSH2_CHANNEL* ch)
{
    const int fd = ConnectToLocalX11Display();
    if (fd < 0) {
        // No local X server; reject the channel so the remote app gets an error.
        libssh2_channel_free(ch);
        return;
    }
    x11_channels_.push_back({.channel = ch, .local_fd = fd});
}

// Delegates to ConnectToX11Display() using the current process's $DISPLAY.
int SshTransport::ConnectToLocalX11Display()
{
    return ConnectToX11Display(::getenv("DISPLAY"));
}

// Called from AgentOpenCallback when the server opens an auth-agent channel.
// Connects to the local SSH agent and records the proxy pair for ReadWriteLoop.
void SshTransport::AcceptAgentChannel(LIBSSH2_CHANNEL* ch)
{
    const int fd = ConnectToLocalSshAgent();
    if (fd < 0) {
        libssh2_channel_free(ch);
        return;
    }
    agent_channels_.push_back({.channel = ch, .local_fd = fd});
}

// Opens a Unix domain socket to $SSH_AUTH_SOCK.
// Returns the fd on success, -1 if no agent is available.
int SshTransport::ConnectToLocalSshAgent()
{
    const char* sock = ::getenv("SSH_AUTH_SOCK");
    if (!sock || sock[0] == '\0') return -1;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool SshTransport::PollUntilReady(int timeout_ms)
{
    return ssh::PollUntilReady(session_, sock_fd_, timeout_ms, running_);
}

void SshTransport::NotifyError(TransportError::Category category,
                               const std::string& msg)
{
    target_.OnData("\r\n\x1b[31m" + msg + "\x1b[0m\r\n");
    target_.OnError(TransportError{category, msg});
    target_.OnDisconnect(DisconnectReason::Interrupted);
    running_ = false;
}

std::string SshTransport::LastSshError() const
{
    return ssh::LastSshError(session_);
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

std::string SshTransport::GetRemoteDescription() const
{
    return desc_.username + "@" + desc_.host;
}

// ---------------------------------------------------------------------------
// Port Forwarding — thin delegation to PortForwardEngine
// ---------------------------------------------------------------------------

void SshTransport::AddPortForward(const PortForwardDesc& desc)
{
    pfwEngine_.AddForward(desc);
}

void SshTransport::RemovePortForward(PortForwardId id)
{
    pfwEngine_.RemoveForward(id);
}

} // namespace term::transport
