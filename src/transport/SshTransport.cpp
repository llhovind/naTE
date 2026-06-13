#include "transport/SshTransport.h"
#include "transport/EnvUtils.h"
#include "transport/SshConfig.h"
#include "transport/SshPublicKey.h"
#include "transport/X11Utils.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
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

constexpr char kTermType[]            = "xterm-256color";
constexpr int  kPollTimeoutMs         = 100;
constexpr int  kCwdCaptureIntervalSec = 120;

// TCP keepalive parameters applied to every established SSH socket.
// The kernel sends the first probe after kTcpKeepIdleSec of silence, then
// retries every kTcpKeepIntvlSec up to kTcpKeepCnt times before declaring the
// connection dead.  Total worst-case detection time:
//   kTcpKeepIdleSec + kTcpKeepCnt * kTcpKeepIntvlSec = 10 + 3*10 = 40 s.
constexpr int  kTcpKeepIdleSec  = 10;
constexpr int  kTcpKeepIntvlSec = 10;
constexpr int  kTcpKeepCnt      = 3;

// Returns preferred public-key blobs for agent auth derived from the connection's
// agentIdentityHint (first priority) or ~/.ssh/config lookup (second priority).
// Used by both the main-session and aux-session agent auth paths.
std::vector<std::vector<uint8_t>> LoadPreferredBlobs(
    const term::transport::SshDesc& desc)
{
    std::vector<std::filesystem::path> paths;

    if (!desc.agentIdentityHint.empty()) {
        std::filesystem::path p(desc.agentIdentityHint);
        if (p.extension() != ".pub") p += ".pub";
        paths.push_back(std::move(p));
    } else {
        paths = QuerySshConfigIdentities(desc.host, desc.port, desc.username);
        for (auto& p : paths)
            if (p.extension() != ".pub") p += ".pub";
    }

    std::vector<std::vector<uint8_t>> blobs;
    blobs.reserve(paths.size());
    for (const auto& p : paths) {
        auto blob = LoadPublicKeyBlob(p);
        if (!blob.empty())
            blobs.push_back(std::move(blob));
    }
    return blobs;
}

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

// Shared keyboard-interactive response logic.  Builds a KbdIntChallenge from
// the libssh2 prompt arrays, delegates to target for user responses, then fills
// in libssh2's response structs.  libssh2 owns the response buffers and frees
// them with free(), so strdup() is the correct allocator here.
void ApplyKbdIntResponses(
    const char* name,        int name_len,
    const char* instruction, int instruction_len,
    int num_prompts,
    const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
    LIBSSH2_USERAUTH_KBDINT_RESPONSE*     responses,
    term::transport::ITransportTarget&    target)
{
    term::transport::KbdIntChallenge challenge;
    challenge.name        = std::string(name,        static_cast<size_t>(name_len));
    challenge.instruction = std::string(instruction, static_cast<size_t>(instruction_len));
    for (int i = 0; i < num_prompts; ++i)
        challenge.prompts.push_back({
            std::string(reinterpret_cast<const char*>(prompts[i].text),
                        prompts[i].length),
            prompts[i].echo != 0
        });

    const auto answers = target.OnKbdIntChallenge(challenge);

    for (int i = 0; i < num_prompts; ++i) {
        const std::string& ans =
            (i < static_cast<int>(answers.size())) ? answers[i] : "";
        responses[i].text   = strdup(ans.c_str());
        responses[i].length = static_cast<unsigned int>(ans.size());
    }
}

// Keyboard-interactive callback for the main worker-thread session.
// abstract is the session user pointer, set to `this` (SshTransport*) in WorkerThread.
void KbdIntCallback(
    const char* name,        int name_len,
    const char* instruction, int instruction_len,
    int num_prompts,
    const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
    LIBSSH2_USERAUTH_KBDINT_RESPONSE*     responses,
    void**                                abstract)
{
    auto* self = static_cast<term::transport::SshTransport*>(*abstract);
    ApplyKbdIntResponses(name, name_len, instruction, instruction_len,
                         num_prompts, prompts, responses, self->Target());
}

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
      if (!VerifyHostKey(session_, khErr)) {
          NotifyError(TransportError::Category::HostKey, khErr);
          return;
      }
    }
    // Store `this` in the session abstract slot before Authenticate() so that
    // KbdIntCallback (and later X11/agent callbacks) can reach this instance.
    *libssh2_session_abstract(session_) = this;

    if (!Authenticate())                return;

    // Register X11 callback — abstract already set above.
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
    // send SSH_MSG_GLOBAL_REQUEST every keepaliveSeconds seconds, which causes
    // TCP to switch from keepalive-probe mode into retransmit mode and prevents
    // ETIMEDOUT from being raised within the expected window.

    if (desc_.compress)
        libssh2_session_flag(session_, LIBSSH2_FLAG_COMPRESS, 1);

    const auto disconnectReason = ReadWriteLoop();

    // Signal cancellation so pending SFTP tasks see !running_ and self-cancel.
    running_.store(false);
    {
        std::deque<SftpTask> pending;
        {
            std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
            pending.swap(sftp_queue_);
        }
        for (auto& task : pending)
            task();  // task checks !running_, calls onDone(false,...), returns false
    }

    // For Interrupted (dead socket), SO_ERROR was consumed by the recv() that
    // detected the failure.  Subsequent poll() calls on the socket may no longer
    // return POLLERR, causing any blocking libssh2 operation to wait indefinitely
    // for a server response that will never come.  Skip all network-sending
    // teardown steps; just free the in-memory resources and close the fd.
    const bool socketDead = (disconnectReason == DisconnectReason::Interrupted);

    // Best-effort teardown operations while the socket is still alive.
    if (!socketDead && session_) {
        libssh2_session_set_blocking(session_, 1);

        if (sftp_) {
            libssh2_sftp_shutdown(sftp_);
            sftp_ = nullptr;
        }

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
    if (sftp_) {
        // sftp_ was not shut down cleanly (socket was dead); free local resources.
        libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
    }
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
    using AM = term::transport::SshAuthMethod;
    switch (desc_.authMethod) {
        case AM::Agent:          return AuthViaAgent();
        case AM::Password:       return AuthViaPassword();
        case AM::PrivateKey:     return AuthViaPrivateKey();
        case AM::KbdInteractive: return AuthViaKbdInteractive();
    }
    return false;
}

std::vector<std::vector<uint8_t>> SshTransport::PreferredAgentKeyBlobs() const
{
    return LoadPreferredBlobs(desc_);
}

bool SshTransport::AgentTryPreferred(_LIBSSH2_AGENT* agent,
                                     const std::vector<std::vector<uint8_t>>& preferred,
                                     bool* anyMatched)
{
    *anyMatched = false;
    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* prev     = nullptr;

    while (running_) {
        int rc = libssh2_agent_get_identity(agent, &identity, prev);
        if (rc != 0) break;  // rc==1: exhausted; rc<0: error

        // Check if this identity's blob matches any preferred blob.
        const bool matches = std::any_of(
            preferred.begin(), preferred.end(),
            [&](const std::vector<uint8_t>& b) {
                return b.size() == identity->blob_len &&
                       std::memcmp(b.data(), identity->blob, b.size()) == 0;
            });

        if (!matches) { prev = identity; continue; }
        *anyMatched = true;

        int auth;
        while ((auth = libssh2_agent_userauth(agent, desc_.username.c_str(), identity))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return false;
            PollUntilReady(kPollTimeoutMs);
        }
        if (auth == 0) return true;

        prev = identity;
    }
    return false;
}

bool SshTransport::AgentTryAll(_LIBSSH2_AGENT* agent)
{
    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* prev     = nullptr;

    while (running_) {
        int rc = libssh2_agent_get_identity(agent, &identity, prev);
        if (rc == 1) {
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
        while ((auth = libssh2_agent_userauth(agent, desc_.username.c_str(), identity))
               == LIBSSH2_ERROR_EAGAIN) {
            if (!running_) return false;
            PollUntilReady(kPollTimeoutMs);
        }
        if (auth == 0) return true;

        prev = identity;
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

    const auto preferred = PreferredAgentKeyBlobs();
    if (!preferred.empty()) {
        bool anyMatched = false;
        if (AgentTryPreferred(agent_, preferred, &anyMatched)) return true;
        if (anyMatched) {
            // The SSH config / hint key was in the agent but was rejected — do not
            // spray the remaining keys at a server with strict MaxAuthTries.
            NotifyError(TransportError::Category::Authentication,
                        "SSH: preferred identity (from SSH config or hint) was not accepted by the server");
            return false;
        }
        // Preferred keys weren't in the agent at all — fall back to trying all.
    }

    return AgentTryAll(agent_);
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

bool SshTransport::AuthViaKbdInteractive()
{
    int rc;
    while ((rc = libssh2_userauth_keyboard_interactive(
                session_,
                desc_.username.c_str(),
                &KbdIntCallback)) == LIBSSH2_ERROR_EAGAIN) {
        if (!running_) return false;
        PollUntilReady(kPollTimeoutMs);
    }
    if (rc != 0) {
        NotifyError(TransportError::Category::Authentication,
                    "SSH: keyboard-interactive authentication failed — " + LastSshError());
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
    char buf[kReadBuf];

    DisconnectReason reason = DisconnectReason::Deliberate;

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
            const size_t pfwBase = pfds.size();
            std::vector<PfwPollEntry> pfwTags;
            BuildPortForwardPollFds(pfds, pfwTags);

            ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), kPollTimeoutMs);

            if (!running_) break;

            // Service local X11 / agent sockets → their SSH channels.
            for (size_t i = 0; i < x11_channels_.size(); ++i)
                x11_channels_[i].PumpLocalToChannel(pfds[1 + i].revents, buf, kReadBuf);

            const size_t agentBase = 1 + x11_channels_.size();
            for (size_t i = 0; i < agent_channels_.size(); ++i)
                agent_channels_[i].PumpLocalToChannel(pfds[agentBase + i].revents, buf, kReadBuf);

            // Service port forward listen fds and proxy connections (both directions).
            ServicePortForwardConns(pfds, pfwBase, pfwTags, buf, kReadBuf);
        }

        // --- Read from main shell channel ---------------------------------
        ssize_t nRead;
        while ((nRead = libssh2_channel_read(channel_, buf, kReadBuf)) > 0)
            target_.OnData(std::string(buf, static_cast<size_t>(nRead)));

        if (nRead == 0 || libssh2_channel_eof(channel_)) {
            reason = DisconnectReason::Clean;
            break;
        }
        if (nRead != LIBSSH2_ERROR_EAGAIN && nRead < 0) {
            reason = DisconnectReason::Interrupted;
            break;
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
        ServiceSftpQueue();
        ServicePortForwardQueue();

    }

    ReleaseAllProxies(x11_channels_);
    ReleaseAllProxies(agent_channels_);

    // Close all port forward listeners and proxy connections.
    for (auto& fwd : local_fwds_) {
        ::close(fwd.listen_fd);
        ReleaseAllProxies(fwd.conns);
    }
    local_fwds_.clear();
    for (auto& fwd : remote_fwds_) {
        libssh2_channel_forward_cancel(fwd.listener);
        ReleaseAllProxies(fwd.conns);
    }
    remote_fwds_.clear();

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
    target_.OnDisconnect(DisconnectReason::Interrupted);
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
// File send/receive — public interface
// ---------------------------------------------------------------------------

std::string SshTransport::GetRemoteDescription() const
{
    return desc_.username + "@" + desc_.host;
}

// ---------------------------------------------------------------------------
// SFTP task state machines (nested types — have access to SshTransport privates)
// ---------------------------------------------------------------------------

// Formats a POSIX permission bitmask as a "drwxr-xr-x" style string.
static std::string SftpFormatPermissions(unsigned long mode)
{
    char buf[11];
    buf[0] = LIBSSH2_SFTP_S_ISDIR(mode) ? 'd' :
             LIBSSH2_SFTP_S_ISLNK(mode) ? 'l' : '-';
    const unsigned long bits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    const char         chars[3] = {'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i)
        buf[1 + i] = (mode & bits[i]) ? chars[i % 3] : '-';
    buf[10] = '\0';
    return buf;
}

// Formats a Unix timestamp as "YYYY-MM-DD HH:MM".
static std::string SftpFormatModTime(unsigned long mtime)
{
    char buf[32];
    const time_t t = static_cast<time_t>(mtime);
    struct tm tm{};
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

struct SshTransport::SftpListDirTask {
    enum class State { InitSftp, OpenDir, ReadLoop };

    SshTransport* self  = nullptr;
    State         state = State::InitSftp;
    std::string   path;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    std::vector<RemoteDirEntry> entries;
    std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone;

    SftpListDirTask() = default;
    SftpListDirTask(SftpListDirTask&&) = default;
    SftpListDirTask& operator=(SftpListDirTask&&) = default;
    SftpListDirTask(const SftpListDirTask&) = delete;
    SftpListDirTask& operator=(const SftpListDirTask&) = delete;

    ~SftpListDirTask() { if (handle) libssh2_sftp_closedir(handle); }

    bool operator()()
    {
        if (!self->running_) {
            if (handle) { libssh2_sftp_closedir(handle); handle = nullptr; }
            onDone({}, "Session closed");
            return false;
        }

        if (state == State::InitSftp) {
            if (!self->sftp_) {
                LIBSSH2_SFTP* s = libssh2_sftp_init(self->session_);
                if (!s) {
                    if (libssh2_session_last_errno(self->session_) == LIBSSH2_ERROR_EAGAIN)
                        return true;
                    onDone({}, "SFTP unavailable: " + self->LastSshError());
                    return false;
                }
                self->sftp_ = s;
            }
            state = State::OpenDir;
        }

        if (state == State::OpenDir) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_opendir(self->sftp_, path.c_str());
            if (!h) {
                if (libssh2_session_last_errno(self->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                onDone({}, "Cannot list '" + path + "': " + self->LastSshError());
                return false;
            }
            handle = h;
            state  = State::ReadLoop;
        }

        // ReadLoop: drain all available entries this iteration.
        char namebuf[512];
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        while (true) {
            const int rc = libssh2_sftp_readdir_ex(
                handle, namebuf, sizeof(namebuf) - 1, nullptr, 0, &attrs);
            if (rc == LIBSSH2_ERROR_EAGAIN) return true;
            if (rc == 0) {
                libssh2_sftp_closedir(handle); handle = nullptr;
                auto cb   = std::move(onDone);
                auto ents = std::move(entries);
                cb(std::move(ents), {});
                return false;
            }
            if (rc < 0) {
                libssh2_sftp_closedir(handle); handle = nullptr;
                onDone({}, "Directory read error: " + self->LastSshError());
                return false;
            }
            namebuf[rc] = '\0';
            std::string name(namebuf, static_cast<size_t>(rc));
            if (name == "." || name == "..") continue;

            RemoteDirEntry e;
            e.name = std::move(name);
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
                e.size = attrs.filesize;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
                e.isDir       = LIBSSH2_SFTP_S_ISDIR(attrs.permissions) != 0;
                e.isSymlink   = LIBSSH2_SFTP_S_ISLNK(attrs.permissions) != 0;
                e.permissions = SftpFormatPermissions(attrs.permissions);
            }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                e.modTime = SftpFormatModTime(attrs.mtime);
            entries.push_back(std::move(e));
        }
    }
};

struct SshTransport::SftpDownloadTask {
    enum class State { InitSftp, OpenHandle, ReadLoop };

    SshTransport* self  = nullptr;
    State         state = State::InitSftp;
    std::string   remotePath;
    std::string   localPath;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    std::ofstream out;
    std::function<void(bool, std::string)> onDone;

    SftpDownloadTask() = default;
    SftpDownloadTask(SftpDownloadTask&&) = default;
    SftpDownloadTask& operator=(SftpDownloadTask&&) = default;
    SftpDownloadTask(const SftpDownloadTask&) = delete;
    SftpDownloadTask& operator=(const SftpDownloadTask&) = delete;

    ~SftpDownloadTask() { if (handle) libssh2_sftp_close(handle); }

    bool operator()()
    {
        if (!self->running_) {
            if (handle) { libssh2_sftp_close(handle); handle = nullptr; }
            onDone(false, "Session closed");
            return false;
        }

        if (state == State::InitSftp) {
            if (!self->sftp_) {
                LIBSSH2_SFTP* s = libssh2_sftp_init(self->session_);
                if (!s) {
                    if (libssh2_session_last_errno(self->session_) == LIBSSH2_ERROR_EAGAIN)
                        return true;
                    onDone(false, "SFTP unavailable: " + self->LastSshError());
                    return false;
                }
                self->sftp_ = s;
            }
            out.open(localPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                onDone(false, "Cannot create local file: " + localPath);
                return false;
            }
            state = State::OpenHandle;
        }

        if (state == State::OpenHandle) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                self->sftp_, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
            if (!h) {
                if (libssh2_session_last_errno(self->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                onDone(false, "Cannot open '" + remotePath + "': " +
                              self->LastSshError());
                return false;
            }
            handle = h;
            state  = State::ReadLoop;
        }

        // ReadLoop: drain all available data this iteration.
        char buf[32768];
        while (true) {
            const ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) return true;
            if (n < 0) {
                libssh2_sftp_close(handle); handle = nullptr;
                onDone(false, "Read error: " + self->LastSshError());
                return false;
            }
            if (n == 0) {
                libssh2_sftp_close(handle); handle = nullptr;
                out.close();
                onDone(true, localPath);
                return false;
            }
            out.write(buf, n);
        }
    }
};

struct SshTransport::SftpUploadTask {
    enum class State { InitSftp, OpenHandle, WriteLoop };

    SshTransport* self  = nullptr;
    State         state = State::InitSftp;
    std::string   localPath;
    std::string   remotePath;
    LIBSSH2_SFTP_HANDLE* handle  = nullptr;
    std::ifstream in;
    char          buf[32768]{};
    size_t        bufLen  = 0;
    size_t        bufSent = 0;
    std::function<void(bool, std::string)> onDone;

    SftpUploadTask() = default;
    SftpUploadTask(SftpUploadTask&&) = default;
    SftpUploadTask& operator=(SftpUploadTask&&) = default;
    SftpUploadTask(const SftpUploadTask&) = delete;
    SftpUploadTask& operator=(const SftpUploadTask&) = delete;

    ~SftpUploadTask() { if (handle) libssh2_sftp_close(handle); }

    bool operator()()
    {
        if (!self->running_) {
            if (handle) { libssh2_sftp_close(handle); handle = nullptr; }
            onDone(false, "Session closed");
            return false;
        }

        if (state == State::InitSftp) {
            if (!self->sftp_) {
                LIBSSH2_SFTP* s = libssh2_sftp_init(self->session_);
                if (!s) {
                    if (libssh2_session_last_errno(self->session_) == LIBSSH2_ERROR_EAGAIN)
                        return true;
                    onDone(false, "SFTP unavailable: " + self->LastSshError());
                    return false;
                }
                self->sftp_ = s;
            }
            in.open(localPath, std::ios::binary);
            if (!in) {
                onDone(false, "Cannot open local file: " + localPath);
                return false;
            }
            state = State::OpenHandle;
        }

        if (state == State::OpenHandle) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                self->sftp_, remotePath.c_str(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);  // 0644
            if (!h) {
                if (libssh2_session_last_errno(self->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                onDone(false, "Cannot open remote '" + remotePath + "' for write: " +
                              self->LastSshError());
                return false;
            }
            handle = h;
            state  = State::WriteLoop;
        }

        // WriteLoop: send buffered data; read next chunk when buffer exhausted.
        while (true) {
            if (bufSent >= bufLen) {
                in.read(buf, sizeof(buf));
                bufLen  = static_cast<size_t>(in.gcount());
                bufSent = 0;
                if (bufLen == 0) {
                    libssh2_sftp_close(handle); handle = nullptr;
                    onDone(true, {});
                    return false;
                }
            }
            const ssize_t n = libssh2_sftp_write(handle, buf + bufSent, bufLen - bufSent);
            if (n == LIBSSH2_ERROR_EAGAIN) return true;
            if (n < 0) {
                libssh2_sftp_close(handle); handle = nullptr;
                onDone(false, "Write error: " + self->LastSshError());
                return false;
            }
            bufSent += static_cast<size_t>(n);
        }
    }
};

// ---------------------------------------------------------------------------
// ServiceSftpQueue — advances the front SFTP task by one step.
// Must be called only from the worker thread.
// ---------------------------------------------------------------------------

void SshTransport::ServiceSftpQueue()
{
    SftpTask task;
    {
        std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
        if (sftp_queue_.empty()) return;
        task = std::move(sftp_queue_.front());
        sftp_queue_.pop_front();
    }
    const bool again = task();
    if (again) {
        std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
        sftp_queue_.push_front(std::move(task));
    }
}

// ---------------------------------------------------------------------------
// SFTP public dispatch methods
// ---------------------------------------------------------------------------

void SshTransport::SendFile(const std::string& localPath,
                            const std::string& remoteDir,
                            std::function<void(bool, std::string)> onDone)
{
    std::string filename = localPath;
    if (const auto pos = filename.rfind('/'); pos != std::string::npos)
        filename = filename.substr(pos + 1);
    std::string remotePath = remoteDir;
    if (!remotePath.empty() && remotePath.back() != '/') remotePath += '/';
    remotePath += filename;

    SftpUploadTask t;
    t.self       = this;
    t.localPath  = localPath;
    t.remotePath = std::move(remotePath);
    t.onDone     = std::move(onDone);
    auto ptr = std::make_shared<SftpUploadTask>(std::move(t));
    std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
    sftp_queue_.emplace_back([ptr]() mutable { return (*ptr)(); });
}

// ---------------------------------------------------------------------------
// Remaining SFTP dispatch methods
// ---------------------------------------------------------------------------

void SshTransport::ReceiveFile(const std::string& remotePath,
                               const std::string& localDir,
                               std::function<void(bool, std::string)> onDone)
{
    std::string filename = remotePath;
    if (const auto pos = filename.rfind('/'); pos != std::string::npos)
        filename = filename.substr(pos + 1);
    std::string localPath = localDir;
    if (!localPath.empty() && localPath.back() != '/') localPath += '/';
    localPath += filename;

    SftpDownloadTask t;
    t.self       = this;
    t.remotePath = remotePath;
    t.localPath  = std::move(localPath);
    t.onDone     = std::move(onDone);
    auto ptr = std::make_shared<SftpDownloadTask>(std::move(t));
    std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
    sftp_queue_.emplace_back([ptr]() mutable { return (*ptr)(); });
}

void SshTransport::ListRemoteDirectory(
    const std::string& remotePath,
    std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone)
{
    SftpListDirTask t;
    t.self   = this;
    t.path   = remotePath;
    t.onDone = std::move(onDone);
    auto ptr = std::make_shared<SftpListDirTask>(std::move(t));
    std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
    sftp_queue_.emplace_back([ptr]() mutable { return (*ptr)(); });
}

void SshTransport::SftpDownloadFile(const std::string& remotePath,
                                    const std::string& localPath,
                                    std::function<void(bool, std::string)> onDone)
{
    SftpDownloadTask t;
    t.self       = this;
    t.remotePath = remotePath;
    t.localPath  = localPath;
    t.onDone     = std::move(onDone);
    auto ptr = std::make_shared<SftpDownloadTask>(std::move(t));
    std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
    sftp_queue_.emplace_back([ptr]() mutable { return (*ptr)(); });
}

void SshTransport::SftpUploadFile(const std::string& localPath,
                                  const std::string& remotePath,
                                  std::function<void(bool, std::string)> onDone)
{
    SftpUploadTask t;
    t.self       = this;
    t.localPath  = localPath;
    t.remotePath = remotePath;
    t.onDone     = std::move(onDone);
    auto ptr = std::make_shared<SftpUploadTask>(std::move(t));
    std::lock_guard<std::mutex> lk(sftp_queue_mutex_);
    sftp_queue_.emplace_back([ptr]() mutable { return (*ptr)(); });
}

// ---------------------------------------------------------------------------
// Port Forwarding — public API (UI thread)
// ---------------------------------------------------------------------------

void SshTransport::AddPortForward(const PortForwardDesc& desc)
{
    std::lock_guard<std::mutex> lk(pfw_mutex_);
    pfw_pending_.push_back(PfwAdd{desc});
}

void SshTransport::RemovePortForward(PortForwardId id)
{
    std::lock_guard<std::mutex> lk(pfw_mutex_);
    pfw_pending_.push_back(PfwRemove{id});
}

// ---------------------------------------------------------------------------
// Port Forwarding — worker-thread helpers
// ---------------------------------------------------------------------------

std::string SshTransport::ErrnoString(int err)
{
    char buf[256];
    return ::strerror_r(err, buf, sizeof(buf));
}

void SshTransport::NotifyPortForwardStatus()
{
    std::vector<PortForwardStatus> status;
    for (const auto& fwd : local_fwds_) {
        PortForwardStatus s;
        s.id          = fwd.desc.id;
        s.active      = fwd.listen_fd >= 0;
        s.connections = fwd.conns.size();
        status.push_back(std::move(s));
    }
    for (const auto& fwd : remote_fwds_) {
        PortForwardStatus s;
        s.id          = fwd.desc.id;
        s.active      = fwd.listener != nullptr;
        s.connections = fwd.conns.size();
        status.push_back(std::move(s));
    }
    // Append persisted failures so they remain visible until explicitly removed.
    for (const auto& [id, error] : pfw_failed_) {
        PortForwardStatus s;
        s.id     = id;
        s.active = false;
        s.error  = error;
        status.push_back(std::move(s));
    }

    if (status == pfw_last_status_) return;
    pfw_last_status_ = status;
    target_.OnPortForwardStatusChanged(std::move(status));
}

void SshTransport::FireFailedStatus(PortForwardId id, const std::string& error)
{
    pfw_failed_[id] = error;
    NotifyPortForwardStatus();
}

void SshTransport::ServicePortForwardQueue()
{
    std::vector<PfwPending> pending;
    {
        std::lock_guard<std::mutex> lk(pfw_mutex_);
        pending.swap(pfw_pending_);
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
                        FireFailedStatus(desc.id, "getaddrinfo: " + ErrnoString(errno));
                        return;
                    }
                    int fd = ::socket(res->ai_family, SOCK_STREAM, 0);
                    if (fd < 0) {
                        ::freeaddrinfo(res);
                        FireFailedStatus(desc.id, "socket: " + ErrnoString(errno));
                        return;
                    }
                    int on = 1;
                    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
                    if (::bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
                        const std::string msg = "bind: " + ErrnoString(errno);
                        ::close(fd);
                        ::freeaddrinfo(res);
                        FireFailedStatus(desc.id, msg);
                        return;
                    }
                    ::freeaddrinfo(res);
                    ::listen(fd, 16);
                    ::fcntl(fd, F_SETFL, O_NONBLOCK);
                    local_fwds_.push_back({desc, fd, {}});
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
                                &bound, 16);
                        if (lst) break;
                        const int err = libssh2_session_last_error(
                                session_, nullptr, nullptr, 0);
                        if (err != LIBSSH2_ERROR_EAGAIN) {
                            FireFailedStatus(desc.id, "forward-listen: " + LastSshError());
                            return;
                        }
                        if (!PollUntilReady(kPollTimeoutMs)) return;
                    }
                    if (!lst) return;
                    ActiveRemoteFwd rfwd;
                    rfwd.desc       = desc;
                    rfwd.listener   = lst;
                    rfwd.bound_port = bound;
                    remote_fwds_.push_back(std::move(rfwd));
                    changed = true;
                }

            } else if constexpr (std::is_same_v<T, PfwRemove>) {
                const PortForwardId id = v.id;

                // Remove from local forwards.
                auto lit = std::find_if(local_fwds_.begin(), local_fwds_.end(),
                                        [id](const ActiveLocalFwd& f) { return f.desc.id == id; });
                if (lit != local_fwds_.end()) {
                    ::close(lit->listen_fd);
                    ReleaseAllProxies(lit->conns);
                    local_fwds_.erase(lit);
                    pfw_failed_.erase(id);
                    changed = true;
                    return;
                }

                // Remove from remote forwards.
                auto rit = std::find_if(remote_fwds_.begin(), remote_fwds_.end(),
                                        [id](const ActiveRemoteFwd& f) { return f.desc.id == id; });
                if (rit != remote_fwds_.end()) {
                    libssh2_channel_forward_cancel(rit->listener);
                    ReleaseAllProxies(rit->conns);
                    remote_fwds_.erase(rit);
                    pfw_failed_.erase(id);
                    changed = true;
                    return;
                }

                // May be in pfw_failed_ only (setup never succeeded).
                if (pfw_failed_.erase(id)) changed = true;
            }
        }, item);
    }

    if (changed) NotifyPortForwardStatus();
}

void SshTransport::BuildPortForwardPollFds(std::vector<pollfd>& pfds,
                                           std::vector<PfwPollEntry>& tags)
{
    // Listen sockets for local forwards.
    for (size_t i = 0; i < local_fwds_.size(); ++i) {
        pfds.push_back({local_fwds_[i].listen_fd, POLLIN, 0});
        tags.push_back({PfwPollEntry::Kind::LocalListen, i, 0});
    }
    // Proxy connection local sockets for local forwards.
    for (size_t i = 0; i < local_fwds_.size(); ++i) {
        for (size_t j = 0; j < local_fwds_[i].conns.size(); ++j) {
            const auto& c = local_fwds_[i].conns[j];
            pfds.push_back({c.closed ? -1 : c.local_fd, POLLIN, 0});
            tags.push_back({PfwPollEntry::Kind::LocalConn, i, j});
        }
    }
    // Proxy connection local sockets for remote forwards.
    for (size_t i = 0; i < remote_fwds_.size(); ++i) {
        for (size_t j = 0; j < remote_fwds_[i].conns.size(); ++j) {
            const auto& c = remote_fwds_[i].conns[j];
            pfds.push_back({c.closed ? -1 : c.local_fd, POLLIN, 0});
            tags.push_back({PfwPollEntry::Kind::RemoteConn, i, j});
        }
    }
}

void SshTransport::ServicePortForwardConns(const std::vector<pollfd>& pfds,
                                           size_t pfwBase,
                                           const std::vector<PfwPollEntry>& tags,
                                           char* buf, size_t bufLen)
{
    // --- Process tagged poll entries (accept + local→SSH data) ---------------
    for (size_t t = 0; t < tags.size(); ++t) {
        const size_t pfdIdx = pfwBase + t;
        const auto&  tag    = tags[t];
        const short  rev    = pfds[pfdIdx].revents;

        if (tag.kind == PfwPollEntry::Kind::LocalListen) {
            if (!(rev & POLLIN)) continue;
            auto& fwd  = local_fwds_[tag.fwdIdx];
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
                if (!PollUntilReady(kPollTimeoutMs)) { ch = nullptr; break; }
            }
            if (ch) fwd.conns.push_back({.channel = ch, .local_fd = conn});
            else    ::close(conn);  // connection-time failure; forward stays live

        } else {
            // LocalConn or RemoteConn: local→SSH data.
            ChannelProxy& c = (tag.kind == PfwPollEntry::Kind::LocalConn)
                              ? local_fwds_[tag.fwdIdx].conns[tag.connIdx]
                              : remote_fwds_[tag.fwdIdx].conns[tag.connIdx];
            c.PumpLocalToChannel(rev, buf, bufLen);
        }
    }

    // --- Accept new remote-forward connections (SSH-side, non-blocking) ------
    for (auto& fwd : remote_fwds_) {
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
    for (auto& fwd : local_fwds_) {
        PumpChannelsToLocal(fwd.conns, buf, bufLen);
        connChanged |= SweepClosedProxies(fwd.conns);
    }
    for (auto& fwd : remote_fwds_) {
        PumpChannelsToLocal(fwd.conns, buf, bufLen);
        connChanged |= SweepClosedProxies(fwd.conns);
    }

    if (connChanged) NotifyPortForwardStatus();
}

} // namespace term::transport
