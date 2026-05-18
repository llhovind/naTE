#pragma once

#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "transport/TransportError.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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

    // Spawns the worker thread that connects, authenticates, and reads.
    void Start() override;

    // Signals the worker to stop and joins it.
    void Stop() override;

    // Enqueues a PTY resize; worker applies it inside the read/write loop.
    void Resize(unsigned short cols, unsigned short rows) override;

    // Enqueues a remote vpcolumns file update; worker writes it via exec channel.
    void OnViewportColsChanged(unsigned short cols) override;

    bool        SupportsFileTransfer() const noexcept override { return true; }
    std::string GetRemoteDescription() const override;
    void        TransferFile(const std::string& localPath,
                             const std::string& remoteDir,
                             std::function<void(bool, std::string)> onDone) override;
    void        ReceiveFile(const std::string& remotePath,
                            const std::string& localDir,
                            std::function<void(bool, std::string)> onDone) override;
    void        ListRemoteDirectory(
                    const std::string& remotePath,
                    std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone) override;

private:
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
    bool OpenChannel();
    bool RequestPty();
    void ReadWriteLoop();

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

    // Opens a fresh libssh2 session, authenticates, and transfers one file via
    // SCP. Runs on a detached thread; delivers result via wxTheApp->CallAfter.
    void DoTransferFile(std::string localPath,
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

    std::atomic<bool>           running_{false};
    std::thread                 worker_;

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
