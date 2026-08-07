#pragma once
#include "transport/IRemoteFileSystem.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace term::fs {

// ---------------------------------------------------------------------------
// Job model
// ---------------------------------------------------------------------------

enum class TransferDirection { Download, Upload };

enum class JobState {
    Queued,              // waiting its turn
    Checking,            // testing whether the destination already exists
    AwaitingResolution,  // a conflict was found and the user is being asked
    Active,              // bytes are moving
    Completed,
    Failed,
    Cancelled,
    Skipped,             // a conflict was resolved as "leave the existing file"
};

// What to do when the destination already exists.
enum class ConflictPolicy { Ask, Overwrite, Skip, Rename };

// A single answer to a single conflict.
enum class ConflictResolution { Overwrite, Skip, Rename };

using JobId = uint64_t;
inline constexpr JobId kInvalidJobId = 0;

struct TransferJob {
    JobId             id        = kInvalidJobId;
    TransferDirection direction = TransferDirection::Download;

    std::string sourcePath;
    // Rewritten in place when a conflict resolves as Rename, so the view
    // always shows where the bytes actually went.
    std::string destPath;

    uint64_t totalBytes       = 0;
    uint64_t transferredBytes = 0;

    JobState           state = JobState::Queued;
    transport::FsError error;

    // True once the job will never change again.
    bool IsTerminal() const noexcept
    {
        return state == JobState::Completed || state == JobState::Failed ||
               state == JobState::Cancelled || state == JobState::Skipped;
    }
};

// ---------------------------------------------------------------------------
// Collaborators
// ---------------------------------------------------------------------------

// Signals that something changed. Deliberately carries no state: the listener
// queries the queue for current values, so there is no pushed copy to go stale
// between the notification and the repaint.
class ITransferQueueListener {
public:
    virtual ~ITransferQueueListener() = default;
    virtual void OnTransferJobAdded(JobId /*id*/) {}
    virtual void OnTransferJobChanged(JobId /*id*/) {}
    virtual void OnTransferQueueIdle() {}
};

// Marshals a callback onto the thread that owns the queue. The UI supplies
// wxTheApp->CallAfter; tests supply an executor they can drain deliberately.
//
// This is what keeps TransferQueue free of locks: IRemoteFileSystem calls back
// on a worker thread, everything is bounced through here first, and so all
// queue state is touched from exactly one thread.
using Dispatcher = std::function<void(std::function<void()>)>;

// Asks how to resolve a collision. The queue calls this and suspends the job
// until the continuation runs; applyToAll promotes the answer to the standing
// policy for the rest of the queue.
using ConflictPrompt =
    std::function<void(const TransferJob&,
                       std::function<void(ConflictResolution, bool applyToAll)>)>;

// ---------------------------------------------------------------------------
// TransferQueue
// ---------------------------------------------------------------------------

// Sequences file transfers between the local filesystem and one remote.
//
// Jobs run one at a time. Parallel transfers would not go faster — they share
// a single TCP connection with the terminal session — and would make progress
// reporting and cancellation considerably harder to reason about.
//
// The remote endpoint is an IRemoteFileSystem; the local endpoint is
// std::filesystem used directly. That asymmetry is deliberate: Download and
// Upload are the primitives SFTP actually offers, and wrapping the local side
// in a port whose only implementation forwards to std::filesystem would add a
// layer without adding a seam.
class TransferQueue {
public:
    TransferQueue(transport::IRemoteFileSystem& remote, Dispatcher dispatch);
    ~TransferQueue();

    TransferQueue(const TransferQueue&)            = delete;
    TransferQueue& operator=(const TransferQueue&) = delete;

    // Non-owning; must outlive the queue or be cleared first.
    void SetListener(ITransferQueueListener* listener) { listener_ = listener; }

    // Defaults to Ask. With no prompt installed, Ask behaves as Skip — never
    // as Overwrite, because a queue that cannot ask must not destroy data.
    void SetConflictPolicy(ConflictPolicy policy) { policy_ = policy; }
    ConflictPolicy Policy() const noexcept { return policy_; }
    void SetConflictPrompt(ConflictPrompt prompt) { prompt_ = std::move(prompt); }

    // --- Enqueueing ----------------------------------------------------------
    // sizeHint drives the aggregate progress denominator before a transfer
    // starts; pass the size from the directory listing when it is known.
    // Transfers begin immediately — there is no separate start step.
    JobId EnqueueDownload(const std::string& remotePath,
                          const std::string& localPath,
                          uint64_t sizeHint = 0);
    JobId EnqueueUpload(const std::string& localPath,
                        const std::string& remotePath,
                        uint64_t sizeHint = 0);

    // Recursively expands a directory into leaf file jobs, creating the
    // destination directories as it goes. onExpanded reports when the walk
    // finished; the transfers it queued may still be running.
    //
    // Symlinks are never followed. That removes the possibility of a cycle by
    // construction, and matches what rsync does by default — a link to /
    // should not silently turn into a copy of the whole filesystem.
    void EnqueueDownloadTree(const std::string& remoteDir,
                             const std::string& localDir,
                             std::function<void(transport::FsError)> onExpanded);
    void EnqueueUploadTree(const std::string& localDir,
                           const std::string& remoteDir,
                           std::function<void(transport::FsError)> onExpanded);

    // --- Control -------------------------------------------------------------
    // Cancelling an active job asks the transport to stop; the job reaches
    // Cancelled when the transport confirms. Cancelling a queued or suspended
    // job takes effect at once. Unknown or already-terminal ids are ignored.
    void CancelJob(JobId id);
    void CancelAll();

    // Drops every terminal job from the list. Returns the number removed.
    size_t ClearFinished();

    // --- Queries -------------------------------------------------------------
    const std::vector<TransferJob>& Jobs() const noexcept { return jobs_; }
    const TransferJob* FindJob(JobId id) const;

    // Aggregates over all jobs, terminal ones included, so a progress bar does
    // not jump backwards as work completes.
    uint64_t TotalBytes() const;
    uint64_t TransferredBytes() const;

    size_t PendingCount() const;
    bool   IsIdle() const;

private:
    // Everything the transport's callbacks need in order to get back onto the
    // owning thread safely.
    //
    // Captured by value into every callback handed to IRemoteFileSystem, so
    // the callback never touches the TransferQueue before the liveness check
    // has passed on the owning thread. Capturing `this` alone would mean
    // dereferencing a possibly-destroyed object just to reach the dispatcher.
    struct CallbackContext {
        Dispatcher                        post;
        std::weak_ptr<std::atomic<bool>>  alive;
        TransferQueue*                    self = nullptr;

        bool Alive() const
        {
            const auto a = alive.lock();
            return a && a->load(std::memory_order_acquire);
        }
    };

    CallbackContext Context();

    // Advances the queue: starts the next non-terminal job, or reports idle.
    void Pump();

    void BeginJob(JobId id);
    void OnConflictKnown(JobId id, bool exists);
    void ApplyResolution(JobId id, ConflictResolution resolution);
    // Probes for a free destination name, starting at the given attempt
    // number. Recurses through the transport for remote destinations.
    void ResolveFreeName(JobId id, int attempt);
    void StartTransfer(JobId id);
    void OnProgress(JobId id, uint64_t transferred, uint64_t total);
    void OnTransferDone(JobId id, transport::FsError err);
    void FinishJob(JobId id, JobState state, transport::FsError err = {});

    TransferJob* Find(JobId id);
    JobId        AddJob(TransferJob job);

    void NotifyChanged(JobId id);

    transport::IRemoteFileSystem& remote_;
    Dispatcher                    dispatch_;
    ConflictPrompt                prompt_;
    ITransferQueueListener*       listener_ = nullptr;

    std::vector<TransferJob> jobs_;
    JobId                    nextId_  = 1;
    JobId                    activeId_ = kInvalidJobId;
    ConflictPolicy           policy_  = ConflictPolicy::Ask;

    // Transport handle for the in-flight transfer, so CancelJob can reach it.
    transport::TransferHandle activeHandle_ = transport::kInvalidTransferHandle;

    // Cleared on destruction; callbacks in flight check it before touching
    // this object. Shared rather than owned so a callback can hold a weak ref.
    std::shared_ptr<std::atomic<bool>> alive_;
};

// Builds the nth alternative name for a colliding file: "notes.txt" becomes
// "notes (1).txt", then "notes (2).txt". A leading-dot name is treated as
// having no extension, so ".bashrc" becomes ".bashrc (1)" rather than
// "(1).bashrc". Exposed for testing.
std::string MakeUniqueCandidate(const std::string& name, int attempt);

} // namespace term::fs
