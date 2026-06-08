#pragma once

#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "transport/BastionTunnel.h"
#include "transport/PortForward.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "transport/TransportError.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#include <poll.h>

// Forward-declare libssh2 types to keep this header libssh2-free.
struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_AGENT;
struct _LIBSSH2_SFTP;
struct _LIBSSH2_LISTENER;

namespace term::transport {

class SshTransport : public Transport {
public:
    SshTransport(ITransportTarget& target,
                 const term::session::SshDesc& desc,
                 unsigned short cols,
                 unsigned short rows,
                 unsigned short viewportCols,
                 const term::session::SessionInit& sessionInit = {},
                 const term::session::AppSessionDefaults& appDefaults = {});
    ~SshTransport() override;

    SshTransport(const SshTransport&)            = delete;
    SshTransport& operator=(const SshTransport&) = delete;

    // Called from UI/input thread — enqueues data; worker drains it.
    void Write(const std::string& data) override;
    // Ctrl-Q (XON): unstick software flow control if Ctrl-S was accidentally pressed.
    // ESC c (RIS) is intentionally omitted — sent as keyboard input it reaches the
    // remote shell's line editor, not a terminal emulator, and shells like ash/busybox
    // echo the 'c' rather than silently consuming the sequence.
    void SendResetSequence() override { Write("\021"); }

    // Spawns the worker thread that connects, authenticates, and reads.
    void Start() override;

    // Signals the worker to stop and joins it.
    void Stop() override;

    // Enqueues a PTY resize; worker applies it inside the read/write loop.
    void Resize(unsigned short cols, unsigned short rows) override;

    // Enqueues an X11 forwarding request; worker calls libssh2_channel_request_x11_ex
    // inside the read/write loop. No-op if already active or not an SSH channel.
    void RequestX11Forwarding() override;

    // Enqueues a remote vpcolumns file update; worker writes it via exec channel.
    void OnViewportColsChanged(unsigned short cols) override;

    bool        SupportsFileTransfer()    const noexcept override { return true; }
    bool        SupportsX11Forwarding()  const noexcept override { return true; }
    bool        SupportsPortForwarding() const noexcept override { return true; }

    // Thread-safe: may be called from the UI thread at any time after Start().
    void AddPortForward(const PortForwardDesc& desc) override;
    void RemovePortForward(PortForwardId id) override;
    std::string GetRemoteDescription() const override;
    void        SendFile(const std::string& localPath,
                        const std::string& remoteDir,
                        std::function<void(bool, std::string)> onDone) override;
    void        ReceiveFile(const std::string& remotePath,
                            const std::string& localDir,
                            std::function<void(bool, std::string)> onDone) override;
    void        ListRemoteDirectory(
                    const std::string& remotePath,
                    std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone) override;
    void        SftpDownloadFile(const std::string& remotePath,
                                 const std::string& localPath,
                                 std::function<void(bool, std::string)> onDone) override;
    void        SftpUploadFile(const std::string& localPath,
                               const std::string& remotePath,
                               std::function<void(bool, std::string)> onDone) override;

    // Called by the static X11 callback when the server opens an X11 channel.
    // Worker-thread-only; accesses x11_channels_ without locking.
    void AcceptX11Channel(_LIBSSH2_CHANNEL* ch);

    // Called by the static agent callback when the server opens an auth-agent channel.
    // Worker-thread-only; accesses agent_channels_ without locking.
    void AcceptAgentChannel(_LIBSSH2_CHANNEL* ch);

    // Called by KbdIntCallback (file-local static) to reach target_.
    ITransportTarget& Target() { return target_; }

private:
    struct X11Channel {
        _LIBSSH2_CHANNEL* channel  = nullptr;
        int               local_fd = -1;   // socket connected to local X11 server
        bool              closed   = false;
    };

    struct AgentChannel {
        _LIBSSH2_CHANNEL* channel  = nullptr;
        int               local_fd = -1;   // Unix socket connected to $SSH_AUTH_SOCK
        bool              closed   = false;
    };

    struct ProxyConn {
        int               local_fd = -1;
        _LIBSSH2_CHANNEL* channel  = nullptr;
        bool              closed   = false;
    };

    struct ActiveLocalFwd {
        PortForwardDesc        desc;
        int                    listen_fd = -1;
        std::vector<ProxyConn> conns;
    };

    struct ActiveRemoteFwd {
        PortForwardDesc        desc;
        _LIBSSH2_LISTENER*     listener   = nullptr;
        int                    bound_port = 0;
        std::vector<ProxyConn> conns;
    };

    struct PfwAdd    { PortForwardDesc desc; };
    struct PfwRemove { PortForwardId   id;   };

    // Tag paired with each pollfd appended by BuildPortForwardPollFds so that
    // ServicePortForwardConns can dispatch to the right fwd/conn without an
    // implicit shared-order cursor.
    struct PfwPollEntry {
        enum class Kind : uint8_t { LocalListen, LocalConn, RemoteConn };
        Kind   kind;
        size_t fwdIdx;
        size_t connIdx;  // unused for LocalListen
    };

    // Worker thread — owns all libssh2 calls.
    void WorkerThread();

    // Connection sub-steps.  Each returns false and calls NotifyError on failure.
    int  ConnectSocket();
    bool PerformHandshake(int fd);
    // Accepts an explicit session so it can be shared with the SCP transfer path.
    // Returns false and sets outError on failure; does NOT call NotifyError.
    bool VerifyHostKey(_LIBSSH2_SESSION* session, std::string& outError);
    bool Authenticate();
    bool AuthViaAgent();
    bool AuthViaPassword();
    bool AuthViaPrivateKey();
    bool AuthViaKbdInteractive();

    // Returns the public-key blobs that should be tried first from the agent.
    // Checks desc_.agentIdentityHint first; falls back to ~/.ssh/config lookup.
    // Returns empty if no preference is found (caller falls back to trying all keys).
    std::vector<std::vector<uint8_t>> PreferredAgentKeyBlobs() const;

    // Try only agent identities whose blob matches one in `preferred` (non-blocking).
    // Returns true on first success. Caller owns agent lifetime.
    // Sets *anyMatched=true if at least one matching identity was found in the agent.
    bool AgentTryPreferred(_LIBSSH2_AGENT* agent,
                           const std::vector<std::vector<uint8_t>>& preferred,
                           bool* anyMatched);

    // Try all agent identities in order (non-blocking). Returns true on first success.
    bool AgentTryAll(_LIBSSH2_AGENT* agent);
    bool OpenChannel();
    bool RequestPty();
    bool SetupX11Forwarding();
    bool SetupAgentForwarding();
    bool StartShell();
    DisconnectReason ReadWriteLoop();

    // Drains write_queue_ without holding queue_mutex_.
    void DrainWriteQueue();

    // Wraps libssh2_session_block_directions + poll(); returns false if !running_.
    bool PollUntilReady(int timeout_ms);

    // Writes msg into the terminal, fires OnError, then fires OnDisconnect.
    void NotifyError(TransportError::Category category, const std::string& msg);

    // Returns the last libssh2 error string from the session.
    std::string LastSshError() const;

    // Returns the path to ~/.nate/known_hosts, creating the directory if needed.
    static std::string KnownHostsPath();

    // Opens a short-lived exec channel and writes cols to vpcolumns_remote_path_.
    // Must be called only from the worker thread.
    void RemoteWriteVpCols(unsigned short cols);

    // Execs cmd on a short-lived channel and returns trimmed stdout, or "" on
    // failure.  Must be called only from the worker thread (non-blocking mode).
    std::string RemoteExecRead(const std::string& cmd);

    // Runs readlink /proc/{remotePid_}/cwd and fires OnCwdChanged if the
    // capture interval has elapsed.  Must be called only from the worker thread.
    void CaptureCwdPeriodic();

    // Connects a Unix or TCP socket to the local X11 server (parses $DISPLAY).
    // Returns the socket fd on success, -1 if no display is available.
    // Worker-thread-only.
    static int ConnectToLocalX11Display();

    // Connects a Unix socket to the local SSH agent ($SSH_AUTH_SOCK).
    // Returns the socket fd on success, -1 if no agent socket is available.
    // Worker-thread-only.
    static int ConnectToLocalSshAgent();

    // Proxy data between all active X11 channels and their local X11 sockets.
    // Appends to pfds the local X11 socket FDs for poll(); caller provides pfds
    // with the SSH socket already at index 0.
    void ServiceX11Channels(char* buf, size_t bufLen);

    // Advances the front task in sftp_queue_ by one step.
    // Must be called only from the worker thread.
    void ServiceSftpQueue();

    // Drains pfwPending_, sets up/tears down local listeners and remote listeners.
    // Fires OnPortForwardStatusChanged when the set changes.
    // Must be called only from the worker thread.
    void ServicePortForwardQueue();

    // Appends one pollfd+PfwPollEntry pair per listen socket and proxy conn.
    // ServicePortForwardConns takes the same pfwBase and tags to dispatch results
    // without relying on a shared iteration-order contract between the two functions.
    // Must be called only from the worker thread.
    void BuildPortForwardPollFds(std::vector<pollfd>& pfds,
                                 std::vector<PfwPollEntry>& tags);
    void ServicePortForwardConns(const std::vector<pollfd>& pfds, size_t pfwBase,
                                 const std::vector<PfwPollEntry>& tags,
                                 char* buf, size_t bufLen);

    // Collects current status (including any persisted failures) and fires
    // OnPortForwardStatusChanged if the snapshot changed.
    void NotifyPortForwardStatus();

    // Records a permanent failure for id in pfw_failed_ and fires a status update.
    // The failure persists until the forward is explicitly removed via PfwRemove.
    void FireFailedStatus(PortForwardId id, const std::string& error);

    // Returns a human-readable error string for the current errno.
    static std::string ErrnoString(int err);

    // SFTP task state machines — defined in SshTransport.cpp.
    // Declared as nested types so they have access to private members.
    struct SftpListDirTask;
    struct SftpDownloadTask;
    struct SftpUploadTask;

    ITransportTarget&                    target_;
    term::session::SshDesc               desc_;
    term::session::SessionInit           sessionInit_;
    term::session::AppSessionDefaults    appDefaults_;
    unsigned short                       cols_;
    unsigned short                       rows_;
    unsigned short                       viewportCols_;
    std::string                          vpcolumns_remote_path_;

    // libssh2 handles — only accessed from worker_.
    _LIBSSH2_SESSION*           session_  = nullptr;
    _LIBSSH2_CHANNEL*           channel_  = nullptr;
    _LIBSSH2_AGENT*             agent_    = nullptr;
    _LIBSSH2_SFTP*              sftp_     = nullptr;  // lazily initialised on first SFTP op
    int                         sock_fd_  = -1;
    // Non-null when the main session is tunnelled through a ProxyJump host.
    // Destroyed after the worker thread exits so the bridge outlives the session.
    std::unique_ptr<BastionTunnel> bastion_tunnel_;

    std::atomic<bool>           running_{false};
    std::thread                 worker_;

    // X11 forwarding — worker-thread-only after Start().
    std::vector<X11Channel>     x11_channels_;
    std::atomic<bool>           x11_request_pending_{false};
    bool                        x11_active_ = false;

    // SSH agent forwarding — worker-thread-only after Start().
    std::vector<AgentChannel>   agent_channels_;

    // Worker-thread-only: timestamp of the last periodic CWD capture.
    // Default-initialised to epoch so the first capture fires immediately.
    std::chrono::steady_clock::time_point lastCwdCapture_{};
    // PID of the remote shell process, read from the first stdout line at startup.
    int  remotePid_       = 0;
    // Set false on the first readlink failure so we stop trying on non-Linux servers.
    bool procFsAvailable_ = true;
    // Bytes read past the PID newline during startup; drained at ReadWriteLoop start.
    std::string pidOvershoot_;

    // Shared between UI thread (Write/Resize/OnViewportColsChanged) and worker.
    std::mutex                  queue_mutex_;
    std::deque<std::string>     write_queue_;
    bool                        resize_pending_    = false;
    unsigned short              pending_cols_      = 0;
    unsigned short              pending_rows_      = 0;
    bool                        vpcolumns_pending_ = false;
    unsigned short              pending_vpcols_    = 0;

    // SFTP task queue — shared between UI thread (enqueue) and worker (dequeue+advance).
    // Tasks return true to be called again next iteration, false when complete.
    using SftpTask = std::function<bool()>;
    std::mutex                  sftp_queue_mutex_;
    std::deque<SftpTask>        sftp_queue_;

    // Port forward state — worker-thread-owned after Start().
    std::vector<ActiveLocalFwd>  local_fwds_;
    std::vector<ActiveRemoteFwd> remote_fwds_;

    // Pending port forward additions/removals from the UI thread.
    using PfwPending = std::variant<PfwAdd, PfwRemove>;
    std::mutex                   pfw_mutex_;
    std::vector<PfwPending>      pfw_pending_;

    // Last-sent status snapshot; used to suppress redundant callbacks.
    std::vector<PortForwardStatus> pfw_last_status_;

    // Forwards that failed setup: id → error string.  Persisted here so they
    // remain visible in the panel until the user explicitly removes them.
    // Worker-thread-only after Start().
    std::unordered_map<PortForwardId, std::string> pfw_failed_;
};

} // namespace term::transport
