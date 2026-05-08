#pragma once

#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "session/Connection.h"

#include <atomic>
#include <deque>
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
                 unsigned short rows);
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

private:
    // Worker thread — owns all libssh2 calls.
    void WorkerThread();

    // Connection sub-steps.  Each returns false and calls NotifyError on failure.
    int  ConnectSocket();
    bool PerformHandshake(int fd);
    bool VerifyHostKey();
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

    // Writes msg into the terminal then fires OnDisconnect.
    void NotifyError(const std::string& msg);

    // Returns the last libssh2 error string from the session.
    std::string LastSshError() const;

    // Returns the path to ~/.nate/known_hosts, creating the directory if needed.
    static std::string KnownHostsPath();

    ITransportTarget&           target_;
    term::session::SshDesc      desc_;
    unsigned short              cols_;
    unsigned short              rows_;

    // libssh2 handles — only accessed from worker_.
    _LIBSSH2_SESSION*           session_  = nullptr;
    _LIBSSH2_CHANNEL*           channel_  = nullptr;
    _LIBSSH2_AGENT*             agent_    = nullptr;
    int                         sock_fd_  = -1;

    std::atomic<bool>           running_{false};
    std::thread                 worker_;

    // Shared between UI thread (Write/Resize) and worker (DrainWriteQueue).
    std::mutex                  queue_mutex_;
    std::deque<std::string>     write_queue_;
    bool                        resize_pending_ = false;
    unsigned short              pending_cols_   = 0;
    unsigned short              pending_rows_   = 0;
};

} // namespace term::transport
