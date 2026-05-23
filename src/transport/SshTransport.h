#pragma once

#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "transport/BastionTunnel.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "transport/TransportError.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare libssh2 types to keep this header libssh2-free.
struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_AGENT;

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

    bool        SupportsFileTransfer()   const noexcept override { return true; }
    bool        SupportsX11Forwarding()  const noexcept override { return true; }
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

    // Opens a fresh libssh2 session, authenticates, and transfers one file via
    // SCP. Runs on a detached thread; delivers result via wxTheApp->CallAfter.
    void DoSendFile(std::string localPath,
                    std::string remoteDir,
                    std::function<void(bool, std::string)> onDone);

    // Receives one remote file into localDir via SCP on a fresh session.
    void DoReceiveFile(std::string remotePath,
                       std::string localDir,
                       std::function<void(bool, std::string)> onDone);

    // Lists remotePath via an SSH exec channel on a fresh session.
    void DoListRemoteDirectory(
             std::string remotePath,
             std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone);

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

    // Shared between UI thread (Write/Resize/OnViewportColsChanged) and worker.
    std::mutex                  queue_mutex_;
    std::deque<std::string>     write_queue_;
    bool                        resize_pending_    = false;
    unsigned short              pending_cols_      = 0;
    unsigned short              pending_rows_      = 0;
    bool                        vpcolumns_pending_ = false;
    unsigned short              pending_vpcols_    = 0;
};

} // namespace term::transport
