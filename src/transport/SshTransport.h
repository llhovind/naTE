#pragma once

#include "transport/AppSessionDefaults.h"
#include "transport/TransportDesc.h"
#include "transport/BastionTunnel.h"
#include "transport/PortForward.h"
#include "transport/SshAuthenticator.h"
#include "transport/SshChannelProxy.h"
#include "transport/SshPortForwardEngine.h"
#include "transport/SshSftpService.h"
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
#include <vector>

// Forward-declare libssh2 types to keep this header libssh2-free.
struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;
struct _LIBSSH2_AGENT;

namespace term::transport {

class SshTransport : public Transport {
public:
    SshTransport(ITransportTarget& target,
                 const term::transport::SshDesc& desc,
                 unsigned short cols,
                 unsigned short rows,
                 unsigned short viewportCols,
                 const term::transport::SessionInit& sessionInit = {},
                 const term::transport::AppSessionDefaults& appDefaults = {});
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

    bool        SupportsX11Forwarding()  const noexcept override { return true; }
    bool        SupportsPortForwarding() const noexcept override { return true; }

    // Thread-safe: may be called from the UI thread at any time after Start().
    void AddPortForward(const PortForwardDesc& desc) override;
    void RemovePortForward(PortForwardId id) override;
    std::string GetRemoteDescription() const override;

    // SFTP over this session's connection. Non-null for the transport's whole
    // lifetime — the subsystem itself is brought up lazily on first use.
    IRemoteFileSystem* GetRemoteFileSystem() override { return &sftpService_; }

    // Called by the static X11 callback when the server opens an X11 channel.
    // Worker-thread-only; accesses x11_channels_ without locking.
    void AcceptX11Channel(_LIBSSH2_CHANNEL* ch);

    // Called by the static agent callback when the server opens an auth-agent channel.
    // Worker-thread-only; accesses agent_channels_ without locking.
    void AcceptAgentChannel(_LIBSSH2_CHANNEL* ch);

private:
    // Worker thread — owns all libssh2 calls.
    void WorkerThread();

    // Connection sub-steps.  Each returns false and calls NotifyError on failure.
    int  ConnectSocket();
    bool PerformHandshake(int fd);
    // Returns false and sets outError on failure; does NOT call NotifyError.
    bool VerifyHostKey(std::string& outError);
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

    ITransportTarget&                    target_;
    term::transport::SshDesc               desc_;
    term::transport::SessionInit           sessionInit_;
    term::transport::AppSessionDefaults    appDefaults_;
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
    std::vector<ChannelProxy>   x11_channels_;
    std::atomic<bool>           x11_request_pending_{false};
    bool                        x11_active_ = false;

    // SSH agent forwarding — worker-thread-only after Start().
    std::vector<ChannelProxy>   agent_channels_;

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

    // Authentication policy (binds desc_/session_/sock_fd_/running_/agent_/target_).
    SshAuthenticator            authenticator_{desc_, session_, sock_fd_, running_,
                                               agent_, target_};

    // SFTP subsystem + cooperative task queue (binds session_/running_ by ref).
    SftpService                 sftpService_{session_, running_};

    // Local/remote port forwards: listeners, proxies, pending queue, status.
    PortForwardEngine           pfwEngine_{session_, sock_fd_, running_, target_};
};

} // namespace term::transport
