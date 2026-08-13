#pragma once
#include "transport/IRemoteFileSystem.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace term::transport {

// The machine naTE is running on, behind the same port as a remote one.
//
// Satisfying IRemoteFileSystem is what lets the explorer's controller, model
// and view serve the local pane unchanged — the alternative was a parallel
// implementation of browsing, sorting and filtering that would drift.
//
// Threading: metadata operations complete synchronously and invoke their
// callback before returning. There is no worker thread to defer to for those,
// and inventing one would buy nothing for a directory listing. Callers that
// marshal (as ExplorerController does) are unaffected. The caveat is a network
// mount: an unresponsive NFS or SSHFS path will block the calling thread, which
// for the UI means a stall. That is a deliberate trade for the simplicity, and
// the reason the file explorer exists in the first place is to avoid needing
// such mounts.
//
// Transfers are the exception, and they are asynchronous. Copying a large file
// takes as long as it takes, and doing it on the caller's thread would freeze
// the UI for the duration with no progress and no way out. One worker thread,
// started on the first copy and joined at destruction, runs them in turn — the
// same one-at-a-time discipline the transfer queue applies to remote work, and
// for the same reason: parallel copies of one disk are not faster.
//
// Download and Upload are the same operation here. The port defines them
// relative to a remote this adapter does not have, so both mean "copy the first
// path to the second on this disk". That is what makes a local-to-local copy
// fall out of the existing routing rather than needing a case of its own.
class LocalFileSystem final : public IRemoteFileSystem {
public:
    LocalFileSystem() = default;
    // Cancels anything in flight or waiting and joins the worker, so no
    // callback can outlive this object.
    ~LocalFileSystem() override;

    LocalFileSystem(const LocalFileSystem&)            = delete;
    LocalFileSystem& operator=(const LocalFileSystem&) = delete;

    bool IsLocalDisk() const noexcept override { return true; }

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
                            std::optional<uint32_t> sourceMode,
                            ProgressCallback onProgress,
                            DoneCallback onDone) override;
    TransferHandle Upload(const std::string& localPath,
                          const std::string& remotePath,
                          std::optional<uint32_t> sourceMode,
                          ProgressCallback onProgress,
                          DoneCallback onDone) override;
    void Cancel(TransferHandle handle) override;

private:
    // One copy, waiting its turn or running.
    struct CopyJob {
        TransferHandle          handle = kInvalidTransferHandle;
        std::string             from;
        std::string             to;
        std::optional<uint32_t> mode;
        ProgressCallback        onProgress;
        DoneCallback            onDone;
        // Shared with the flag table so Cancel() can reach a job whether it is
        // still queued or already running, without holding the lock while it
        // copies.
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    TransferHandle StartCopy(const std::string& from, const std::string& to,
                             std::optional<uint32_t> mode,
                             ProgressCallback onProgress, DoneCallback onDone);
    void    WorkerLoop();
    // Moves the bytes. Runs unlocked on the worker thread.
    FsError RunCopy(const CopyJob& job);
    // Retires a job: forgets its cancellation flag and reports the outcome.
    void    Finish(const CopyJob& job, FsError err);

    std::mutex              mutex_;
    std::condition_variable wake_;
    std::deque<CopyJob>     queue_;
    std::thread             worker_;
    bool                    stopping_   = false;
    TransferHandle          nextHandle_ = 1;
    // Live cancellation flags by handle. Held here rather than only on the job
    // so Cancel() stays O(1) and works before the job is popped.
    std::unordered_map<TransferHandle, std::shared_ptr<std::atomic<bool>>> live_;
};

} // namespace term::transport
