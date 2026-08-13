#pragma once
#include "transport/IRemoteFileSystem.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

namespace term::transport {

// SFTP adapter for IRemoteFileSystem: owns the lazily-initialised SFTP
// subsystem and the cooperative task queue.
//
// Operations are submitted from the UI thread (the enqueue methods touch only
// the queue under queueMutex_) and advanced one non-blocking step per
// worker-loop iteration via Service(). Everything else is worker-thread-only.
//
// The session pointer is bound by reference: the transport creates the libssh2
// session on the worker thread after this object is constructed, so the service
// reads the live value rather than a stale snapshot.
class SftpService final : public IRemoteFileSystem {
public:
    SftpService(_LIBSSH2_SESSION*& session, const std::atomic<bool>& running);

    SftpService(const SftpService&)            = delete;
    SftpService& operator=(const SftpService&) = delete;

    // --- IRemoteFileSystem (UI-thread safe; callbacks fire on the worker) ----
    void List(const std::string& path, ListCallback onDone) override;
    void RealPath(const std::string& path, PathCallback onDone) override;
    void Stat(const std::string& path, StatCallback onDone) override;
    void ReadLink(const std::string& path, PathCallback onDone) override;
    void MakeDirectory(const std::string& path, uint32_t mode,
                       DoneCallback onDone) override;
    void Remove(const std::string& path, bool isDir, DoneCallback onDone) override;
    void CreateSymlink(const std::string& target, const std::string& linkPath,
                       DoneCallback onDone) override;
    void Rename(const std::string& from, const std::string& to,
                DoneCallback onDone) override;
    void SetPermissions(const std::string& path, uint32_t mode,
                        DoneCallback onDone) override;
    TransferHandle Download(const std::string& remotePath,
                            const std::string& localPath,
                            ProgressCallback onProgress,
                            DoneCallback onDone) override;
    TransferHandle Upload(const std::string& localPath,
                          const std::string& remotePath,
                          ProgressCallback onProgress,
                          DoneCallback onDone) override;
    void Cancel(TransferHandle handle) override;

    // --- Worker-thread API ---------------------------------------------------
    // Advances the front task by one step, then moves it to the back of the
    // queue if it has more work. Round-robin rather than run-to-completion:
    // a large transfer must not stall directory listings behind it.
    void Service();
    // Drains the queue, invoking each task once so it retires itself with
    // FsErrorCode::NotConnected.
    //
    // A task is only run once here and is not re-queued, so one that did not
    // retire would have its callback dropped — breaking the port's promise that
    // every callback fires exactly once. Rather than depend on the caller having
    // cleared running_ first, this latches a stop of its own that the tasks also
    // honour, so draining is correct whatever the caller did.
    void CancelPending();
    // libssh2_sftp_shutdown if initialised; idempotent. Safe to call twice
    // (orderly path then dead-socket fallback), matching the prior teardown.
    void Shutdown();

private:
    struct ListDirTask;
    struct DownloadTask;
    struct UploadTask;
    struct SimpleOpTask;
    struct PathOpTask;
    struct StatTask;

    using Task = std::function<bool()>;

    // Outcome of a single non-blocking attempt to bring up the SFTP subsystem.
    enum class InitResult { Ready, Pending, Failed };

    // True once the service must stop issuing work: either the session went
    // away or CancelPending latched a drain. Every task checks this first, so
    // both routes retire a task the same way.
    bool Stopped() const noexcept
    {
        return !running_.load(std::memory_order_acquire) ||
               cancelling_.load(std::memory_order_acquire);
    }

    // Worker-thread only. Lazily initialises sftp_ one non-blocking step at a
    // time. Returns Pending while libssh2 reports EAGAIN, Ready once the
    // subsystem is up, and Failed (with err populated) on a hard error or once
    // the init deadline elapses — the latter guards against servers that accept
    // the channel but never complete the SFTP handshake (e.g. a missing
    // sftp-server binary), where libssh2_sftp_init would otherwise spin on
    // EAGAIN forever and the transfer would hang with no error surfaced.
    InitResult EnsureSftp(FsError& err);

    // Worker-thread only. Translates the current libssh2/SFTP error state into
    // a typed FsError, prefixing message with context.
    FsError MakeError(const std::string& context) const;

    // Enqueues an already-wrapped task. Thread-safe.
    void Enqueue(Task task);

    // Cancellation registry. Cancel() records a handle here; transfer tasks
    // poll it each step and retire themselves. Kept separate from the queue so
    // cancelling never has to walk or mutate in-flight tasks.
    //
    // Registration is what bounds it. The port lets a caller cancel an unknown
    // or already-finished handle, so without a record of which handles are
    // still live, every such call would leave an entry no task will ever come
    // back to clear.
    void RegisterTransfer(TransferHandle handle);
    bool IsCancelled(TransferHandle handle) const;
    void ForgetCancellation(TransferHandle handle);

    _LIBSSH2_SESSION*&       session_;
    const std::atomic<bool>& running_;
    // One-way latch set by CancelPending. Teardown never resumes, so it is
    // never cleared.
    std::atomic<bool>        cancelling_{false};
    _LIBSSH2_SFTP*           sftp_ = nullptr;
    std::mutex               queueMutex_;
    std::deque<Task>         queue_;
    // Set on the first EAGAIN of an init attempt; cleared once it resolves.
    std::optional<std::chrono::steady_clock::time_point> initDeadline_;

    std::atomic<TransferHandle>      nextHandle_{1};
    // Both sets are bounded by the number of transfers in flight: a handle
    // enters live_ when it is issued and leaves both when its task finishes.
    mutable std::mutex               cancelMutex_;
    std::unordered_set<TransferHandle> live_;
    std::unordered_set<TransferHandle> cancelled_;
};

} // namespace term::transport
