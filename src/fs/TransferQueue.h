#pragma once
#include "fs/Dispatcher.h"
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace term::fs {

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------

// One side of a transfer: a filesystem and a name for it.
//
// Both sides are ports, so the local machine, an SSH session, and any future
// endpoint are interchangeable here. The label exists so the queue can say
// "This computer -> root@host" without knowing what those things are.
struct TransferEndpoint {
    transport::IRemoteFileSystem* fs = nullptr;
    std::string                   label;

    bool Valid() const noexcept { return fs != nullptr; }
    bool IsLocalDisk() const noexcept { return fs && fs->IsLocalDisk(); }
};

// ---------------------------------------------------------------------------
// Job model
// ---------------------------------------------------------------------------

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
    JobId id = kInvalidJobId;

    // Captured at enqueue time, not read from the queue: changing what a pane
    // points at must never redirect work already queued.
    TransferEndpoint source;
    TransferEndpoint destination;

    std::string sourcePath;
    // Rewritten in place when a conflict resolves as Rename, so the view
    // always shows where the bytes actually went.
    std::string destPath;

    uint64_t totalBytes       = 0;
    uint64_t transferredBytes = 0;

    JobState           state = JobState::Queued;
    transport::FsError error;

    // Set when neither endpoint is the local disk. SFTP cannot move bytes
    // server to server, so the file is pulled down and pushed back up, and
    // totalBytes counts both legs — the bytes genuinely do travel twice, and a
    // progress bar that pretended otherwise would stall at 50%.
    bool        viaLocalStaging = false;
    std::string tempPath;
    // Bytes finished in earlier legs, so progress accumulates across them.
    uint64_t    completedLegBytes = 0;

    bool IsTerminal() const noexcept
    {
        return state == JobState::Completed || state == JobState::Failed ||
               state == JobState::Cancelled || state == JobState::Skipped;
    }

    // True when this job moves data to or from the given filesystem.
    bool Uses(const transport::IRemoteFileSystem* fs) const noexcept
    {
        return fs && (source.fs == fs || destination.fs == fs);
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

// Asks how to resolve a collision. The queue calls this and suspends the job
// until the continuation runs; applyToAll promotes the answer to the standing
// policy for the rest of the queue.
using ConflictPrompt =
    std::function<void(const TransferJob&,
                       std::function<void(ConflictResolution, bool applyToAll)>)>;

// ---------------------------------------------------------------------------
// TransferQueue
// ---------------------------------------------------------------------------

// Sequences file transfers between any two endpoints.
//
// Jobs run one at a time. Parallel transfers would not go faster — they share
// a connection with the terminal session — and would make progress reporting
// and cancellation considerably harder to reason about.
//
// Every endpoint is an IRemoteFileSystem, including the local machine. That
// uniformity is what lets conflict detection, name probing and directory
// walking have one implementation each rather than a local branch and a remote
// branch of the same logic.
class TransferQueue {
public:
    explicit TransferQueue(Dispatcher dispatch);
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
    JobId Enqueue(TransferEndpoint source, const std::string& sourcePath,
                  TransferEndpoint destination, const std::string& destPath,
                  uint64_t sizeHint = 0);

    // Recursively expands a directory into leaf file jobs, creating the
    // destination directories as it goes. onExpanded reports when the walk
    // finished; the transfers it queued may still be running.
    //
    // Symlinks are never followed. That removes the possibility of a cycle by
    // construction, and matches what rsync does by default — a link to /
    // should not silently turn into a copy of the whole filesystem.
    void EnqueueTree(TransferEndpoint source, const std::string& sourceDir,
                     TransferEndpoint destination, const std::string& destDir,
                     std::function<void(transport::FsError)> onExpanded);

    // --- Control -------------------------------------------------------------
    // Cancelling an active job asks the transport to stop; the job reaches
    // Cancelled when the transport confirms. Cancelling a queued or suspended
    // job takes effect at once. Unknown or already-terminal ids are ignored.
    void CancelJob(JobId id);
    void CancelAll();

    // Cancels only the jobs touching one filesystem, for when a single session
    // dies and the rest of the queue is still perfectly valid.
    void CancelJobsUsing(const transport::IRemoteFileSystem* fs);

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
    void Pump();

    void BeginJob(JobId id);
    void OnConflictKnown(JobId id, bool exists);
    void ApplyResolution(JobId id, ConflictResolution resolution);
    void ResolveFreeName(JobId id, int attempt);
    void StartTransfer(JobId id);
    // Second leg of a staged remote-to-remote transfer.
    void StartUploadLeg(JobId id);
    void OnProgress(JobId id, uint64_t transferred, uint64_t total);
    void OnLegDone(JobId id, transport::FsError err, bool wasFirstLeg);
    void FinishJob(JobId id, JobState state, transport::FsError err = {});
    // Removes a staging file, best effort: the transfer's own outcome is what
    // the caller asked about.
    void DiscardStagingFile(const TransferJob& job);

    TransferJob* Find(JobId id);
    JobId        AddJob(TransferJob job);
    void         NotifyChanged(JobId id);

    DispatchGuard           guard_;
    ConflictPrompt          prompt_;
    ITransferQueueListener* listener_ = nullptr;

    std::vector<TransferJob> jobs_;
    JobId                    nextId_   = 1;
    JobId                    activeId_ = kInvalidJobId;
    ConflictPolicy           policy_   = ConflictPolicy::Ask;

    // The in-flight transfer, and which endpoint issued it. Both are needed:
    // a handle is only meaningful to the filesystem that returned it, and for
    // a staged transfer that is the source on one leg and the destination on
    // the next.
    transport::TransferHandle     activeHandle_      = transport::kInvalidTransferHandle;
    transport::IRemoteFileSystem* activeHandleOwner_ = nullptr;
};

// Builds the nth alternative name for a colliding file: "notes.txt" becomes
// "notes (1).txt", then "notes (2).txt". A leading-dot name is treated as
// having no extension, so ".bashrc" becomes ".bashrc (1)" rather than
// "(1).bashrc". Exposed for testing.
std::string MakeUniqueCandidate(const std::string& name, int attempt);

} // namespace term::fs
