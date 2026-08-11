#include "fs/TransferQueue.h"
#include "fs/FileMode.h"
#include "fs/RemotePath.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <system_error>

namespace term::fs {

namespace {

// Guards against a pathologically deep tree, and against a server that reports
// a directory containing itself. Symlinks are already skipped, so this is a
// backstop rather than the primary cycle defence.
constexpr int kMaxTreeDepth = 64;

// How many alternative names a Rename resolution will try before giving up.
// A collision that survives this many attempts means something is wrong that
// renaming will not fix.
constexpr int kMaxRenameAttempts = 100;

// --- Recursive tree expansion ----------------------------------------------
// State for EnqueueTree's walk, at namespace scope so the continuation chain
// below stays a sequence of short steps rather than one deeply nested lambda.

struct PendingDir {
    std::string source;
    std::string dest;
    int         depth = 0;
};

struct TreeWalk {
    TransferEndpoint                        source;
    TransferEndpoint                        destination;
    std::deque<PendingDir>                  pending;
    transport::FsError                      firstError;
    std::function<void(transport::FsError)> onExpanded;

    // Hands the walk's outcome back exactly once.
    void Finish(transport::FsError err)
    {
        auto done = std::move(onExpanded);
        onExpanded = nullptr;
        if (done) done(std::move(err));
    }
};

// Turns one directory's listing into queued file jobs and further directories
// to visit. Returns false when the walk should stop, having already reported.
bool AbsorbListing(TransferQueue& queue, TreeWalk& walk, const PendingDir& dir,
                   const std::vector<transport::FileInfo>& entries,
                   transport::FsError err)
{
    // A directory that failed outright stops the walk; one that returned rows
    // alongside an error contributes what it read.
    if (err.Failed() && entries.empty()) {
        walk.Finish(std::move(err));
        return false;
    }
    if (err.Failed() && walk.firstError.Ok()) walk.firstError = std::move(err);

    for (const auto& e : entries) {
        if (e.isSymlink) continue;   // never follow links
        const std::string src = path::Join(dir.source, e.name);
        const std::string dst = path::Join(dir.dest, e.name);

        if (e.isDir) {
            if (dir.depth + 1 < kMaxTreeDepth)
                walk.pending.push_back({src, dst, dir.depth + 1});
        } else {
            queue.Enqueue(walk.source, src, walk.destination, dst, e.size);
        }
    }
    return true;
}

// Scratch path for a transfer that has to be staged through this machine.
std::string MakeStagingPath(JobId id, const std::string& leaf)
{
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = "/tmp";

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (dir / ("nate_relay_" + std::to_string(id) + "_" +
                   std::to_string(stamp) + "_" + leaf)).string();
}

} // namespace

std::string MakeUniqueCandidate(const std::string& name, int attempt)
{
    const std::string suffix = " (" + std::to_string(attempt) + ")";
    const auto dot = name.rfind('.');
    // dot == 0 is a dotfile, not an extension: ".bashrc" must not become
    // "(1).bashrc".
    if (dot == std::string::npos || dot == 0) return name + suffix;
    return name.substr(0, dot) + suffix + name.substr(dot);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TransferQueue::TransferQueue(Dispatcher dispatch)
    : guard_(std::move(dispatch))
{}

// Callbacks still held by a transport check the guard before touching us, so
// destruction must happen on the same thread the dispatcher posts to.
TransferQueue::~TransferQueue() = default;

// ---------------------------------------------------------------------------
// Enqueueing
// ---------------------------------------------------------------------------

JobId TransferQueue::AddJob(TransferJob job)
{
    job.id = nextId_++;
    const JobId id = job.id;
    jobs_.push_back(std::move(job));
    if (listener_) listener_->OnTransferJobAdded(id);
    Pump();
    return id;
}

JobId TransferQueue::Enqueue(TransferEndpoint source, const std::string& sourcePath,
                             TransferEndpoint destination, const std::string& destPath,
                             uint64_t sizeHint)
{
    TransferJob job;
    job.source      = std::move(source);
    job.destination = std::move(destination);
    job.sourcePath  = sourcePath;
    job.destPath    = destPath;
    job.totalBytes  = sizeHint;

    // Neither side is this machine, so the bytes must come down and go back
    // up. They travel twice, and the denominator says so rather than leaving
    // a progress bar apparently stuck halfway.
    job.viaLocalStaging = job.source.Valid() && job.destination.Valid() &&
                          !job.source.IsLocalDisk() && !job.destination.IsLocalDisk();
    if (job.viaLocalStaging) job.totalBytes = sizeHint * 2;

    return AddJob(std::move(job));
}

void TransferQueue::EnqueueTree(TransferEndpoint source, const std::string& sourceDir,
                                TransferEndpoint destination, const std::string& destDir,
                                std::function<void(transport::FsError)> onExpanded)
{
    auto walk = std::make_shared<TreeWalk>();
    walk->source      = std::move(source);
    walk->destination = std::move(destination);
    walk->pending.push_back({sourceDir, destDir, 0});
    walk->onExpanded  = std::move(onExpanded);

    auto step = std::make_shared<std::function<void()>>();
    auto ctx  = guard_.For(this);

    // One directory per turn: create it, list it, absorb what came back, and
    // come round again for whatever that added to the queue.
    *step = [ctx, walk, step]() {
        if (walk->pending.empty()) {
            walk->Finish(walk->firstError);
            return;
        }

        const PendingDir dir = walk->pending.front();
        walk->pending.pop_front();

        // The destination directory has to exist before anything lands in it,
        // including the root of the tree — the listing below enumerates only
        // its contents. An existing directory is the normal case when merging
        // into a tree, so its error is not a failure.
        walk->destination.fs->MakeDirectory(dir.dest, kDefaultDirectoryMode,
            [ctx, walk, step, dir](transport::FsError) {
                ctx.Post([ctx, walk, step, dir](TransferQueue&) {
                    walk->source.fs->List(dir.source,
                        [ctx, walk, step, dir](
                            std::vector<transport::FileInfo> entries,
                            transport::FsError err) {
                            ctx.Post([walk, step, dir, entries = std::move(entries),
                                      err = std::move(err)](TransferQueue& q) mutable {
                                if (AbsorbListing(q, *walk, dir, entries,
                                                  std::move(err)))
                                    (*step)();
                            });
                        });
                });
            });
    };

    (*step)();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

TransferJob* TransferQueue::Find(JobId id)
{
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [id](const TransferJob& j) { return j.id == id; });
    return it == jobs_.end() ? nullptr : &*it;
}

const TransferJob* TransferQueue::FindJob(JobId id) const
{
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
                                 [id](const TransferJob& j) { return j.id == id; });
    return it == jobs_.end() ? nullptr : &*it;
}

void TransferQueue::NotifyChanged(JobId id)
{
    if (listener_) listener_->OnTransferJobChanged(id);
}

void TransferQueue::Pump()
{
    if (activeId_ != kInvalidJobId) return;   // one at a time

    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
        [](const TransferJob& j) { return j.state == JobState::Queued; });
    if (it == jobs_.end()) {
        if (listener_) listener_->OnTransferQueueIdle();
        return;
    }

    activeId_ = it->id;
    BeginJob(activeId_);
}

void TransferQueue::BeginJob(JobId id)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }

    if (!job->source.Valid() || !job->destination.Valid()) {
        FinishJob(id, JobState::Failed,
                  transport::FsError::Make(transport::FsErrorCode::NotConnected,
                                           "Transfer endpoint is no longer available"));
        return;
    }

    job->state = JobState::Checking;
    NotifyChanged(id);

    // Uniform across endpoints: the destination filesystem is asked whether
    // the path is already there, whether it is a disk or a server.
    auto ctx = guard_.For(this);
    job->destination.fs->Stat(job->destPath,
        [ctx, id](transport::FileInfo, transport::FsError err) {
            ctx.Post([id, err = std::move(err)](TransferQueue& q) mutable {
                // Only a definitive "not there" counts as no conflict. Any
                // other error is left to the transfer itself to report against
                // the real operation rather than being second-guessed here.
                const bool exists = err.code != transport::FsErrorCode::NoSuchFile;
                q.OnConflictKnown(id, exists);
            });
        });
}

void TransferQueue::OnConflictKnown(JobId id, bool exists)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }
    if (job->state != JobState::Checking) { activeId_ = kInvalidJobId; Pump(); return; }

    if (!exists) { StartTransfer(id); return; }

    switch (policy_) {
        case ConflictPolicy::Overwrite: StartTransfer(id);                return;
        case ConflictPolicy::Skip:      FinishJob(id, JobState::Skipped);  return;
        case ConflictPolicy::Rename:    ResolveFreeName(id, 1);            return;
        case ConflictPolicy::Ask:       break;
    }

    if (!prompt_) {
        // Nothing can answer, so take the only choice that cannot destroy the
        // user's data.
        FinishJob(id, JobState::Skipped);
        return;
    }

    job->state = JobState::AwaitingResolution;
    NotifyChanged(id);

    auto ctx = guard_.For(this);
    prompt_(*job, [ctx, id](ConflictResolution resolution, bool applyToAll) {
        ctx.Post([id, resolution, applyToAll](TransferQueue& q) {
            TransferJob* j = q.Find(id);
            // The job may have been cancelled while the prompt was open.
            if (!j || j->state != JobState::AwaitingResolution) return;
            if (applyToAll) {
                switch (resolution) {
                    case ConflictResolution::Overwrite: q.policy_ = ConflictPolicy::Overwrite; break;
                    case ConflictResolution::Skip:      q.policy_ = ConflictPolicy::Skip;      break;
                    case ConflictResolution::Rename:    q.policy_ = ConflictPolicy::Rename;    break;
                }
            }
            q.ApplyResolution(id, resolution);
        });
    });
}

void TransferQueue::ApplyResolution(JobId id, ConflictResolution resolution)
{
    switch (resolution) {
        case ConflictResolution::Overwrite: StartTransfer(id);               return;
        case ConflictResolution::Skip:      FinishJob(id, JobState::Skipped); return;
        case ConflictResolution::Rename:    ResolveFreeName(id, 1);          return;
    }
}

void TransferQueue::ResolveFreeName(JobId id, int attempt)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }

    if (attempt > kMaxRenameAttempts) {
        FinishJob(id, JobState::Failed,
                  transport::FsError::Make(
                      transport::FsErrorCode::AlreadyExists,
                      "Could not find a free name for '" + job->destPath + "'"));
        return;
    }

    const std::string candidate =
        path::Join(path::Parent(job->destPath),
                   MakeUniqueCandidate(path::Leaf(job->destPath), attempt));

    auto ctx = guard_.For(this);
    job->destination.fs->Stat(candidate,
        [ctx, id, candidate, attempt](transport::FileInfo, transport::FsError err) {
            ctx.Post([id, candidate, attempt,
                      err = std::move(err)](TransferQueue& q) mutable {
                if (err.code != transport::FsErrorCode::NoSuchFile) {
                    q.ResolveFreeName(id, attempt + 1);
                    return;
                }
                if (TransferJob* j = q.Find(id)) {
                    j->destPath = candidate;
                    q.NotifyChanged(id);
                }
                q.StartTransfer(id);
            });
        });
}

void TransferQueue::StartTransfer(JobId id)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }

    job->state = JobState::Active;
    NotifyChanged(id);

    auto ctx = guard_.For(this);
    const auto onProgress = [ctx, id](uint64_t done, uint64_t total) {
        ctx.Post([id, done, total](TransferQueue& q) { q.OnProgress(id, done, total); });
    };

    // Download and Upload are defined relative to this machine, so the routing
    // turns on which side — if either — is that machine.
    if (job->destination.IsLocalDisk()) {
        // Remote to here. Also the local-to-local case, which the local
        // adapter declines rather than pretending to support.
        const auto onDone = [ctx, id](transport::FsError err) {
            ctx.Post([id, err = std::move(err)](TransferQueue& q) mutable {
                q.OnLegDone(id, std::move(err), false);
            });
        };
        activeHandleOwner_ = job->source.fs;
        RecordHandle(id, job->source.fs->Download(job->sourcePath, job->destPath,
                                                  onProgress, onDone));
        return;
    }

    if (job->source.IsLocalDisk()) {
        const auto onDone = [ctx, id](transport::FsError err) {
            ctx.Post([id, err = std::move(err)](TransferQueue& q) mutable {
                q.OnLegDone(id, std::move(err), false);
            });
        };
        activeHandleOwner_ = job->destination.fs;
        RecordHandle(id, job->destination.fs->Upload(job->sourcePath, job->destPath,
                                                     onProgress, onDone));
        return;
    }

    // Server to server: pull it down first. SFTP has no server-to-server copy,
    // so the bytes pass through this machine whether or not the user thinks of
    // it that way.
    job->tempPath = MakeStagingPath(id, path::Leaf(job->sourcePath));
    const auto onDone = [ctx, id](transport::FsError err) {
        ctx.Post([id, err = std::move(err)](TransferQueue& q) mutable {
            q.OnLegDone(id, std::move(err), true);
        });
    };
    activeHandleOwner_ = job->source.fs;
    RecordHandle(id, job->source.fs->Download(job->sourcePath, job->tempPath,
                                              onProgress, onDone));
}

// The port allows an adapter to answer inline, in which case the job has
// already retired by the time the handle comes back and the fields describing
// the active transfer belong to whatever runs next. Recording it unconditionally
// would hand a cancel request a handle its owner has long since forgotten.
void TransferQueue::RecordHandle(JobId id, transport::TransferHandle handle)
{
    if (activeId_ != id) return;
    activeHandle_ = handle;
}

void TransferQueue::StartUploadLeg(JobId id)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }

    auto ctx = guard_.For(this);
    const auto onProgress = [ctx, id](uint64_t done, uint64_t total) {
        ctx.Post([id, done, total](TransferQueue& q) { q.OnProgress(id, done, total); });
    };
    const auto onDone = [ctx, id](transport::FsError err) {
        ctx.Post([id, err = std::move(err)](TransferQueue& q) mutable {
            q.OnLegDone(id, std::move(err), false);
        });
    };

    activeHandleOwner_ = job->destination.fs;
    RecordHandle(id, job->destination.fs->Upload(job->tempPath, job->destPath,
                                                 onProgress, onDone));
}

void TransferQueue::OnProgress(JobId id, uint64_t transferred, uint64_t total)
{
    TransferJob* job = Find(id);
    if (!job || job->state != JobState::Active) return;

    job->transferredBytes = job->completedLegBytes + transferred;

    // Trust the transport's figure once it has one: a size hint taken from a
    // directory listing can be stale by the time the bytes actually move.
    if (total) {
        job->totalBytes = job->viaLocalStaging ? total * 2 : total;
    }
    NotifyChanged(id);
}

void TransferQueue::OnLegDone(JobId id, transport::FsError err, bool wasFirstLeg)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }

    if (err.Failed()) {
        DiscardStagingFile(*job);
        const bool cancelled = err.code == transport::FsErrorCode::Cancelled;
        FinishJob(id, cancelled ? JobState::Cancelled : JobState::Failed,
                  std::move(err));
        return;
    }

    if (wasFirstLeg) {
        // Half the bytes are accounted for; the second leg counts from there.
        job->completedLegBytes = job->totalBytes / 2;
        job->transferredBytes  = job->completedLegBytes;
        NotifyChanged(id);
        StartUploadLeg(id);
        return;
    }

    DiscardStagingFile(*job);
    job->transferredBytes = job->totalBytes;
    FinishJob(id, JobState::Completed);
}

void TransferQueue::DiscardStagingFile(const TransferJob& job)
{
    if (job.tempPath.empty()) return;
    std::error_code ec;
    std::filesystem::remove(job.tempPath, ec);
}

void TransferQueue::FinishJob(JobId id, JobState state, transport::FsError err)
{
    if (TransferJob* job = Find(id)) {
        job->state = state;
        job->error = std::move(err);
        job->tempPath.clear();
        NotifyChanged(id);
    }
    if (activeId_ == id) {
        activeId_          = kInvalidJobId;
        activeHandle_      = transport::kInvalidTransferHandle;
        activeHandleOwner_ = nullptr;
    }
    Pump();
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void TransferQueue::CancelJob(JobId id)
{
    TransferJob* job = Find(id);
    if (!job || job->IsTerminal()) return;

    if (job->state == JobState::Active && id == activeId_ && activeHandleOwner_) {
        // The transport owns the job now; it will report Cancelled through the
        // normal completion path, so there is exactly one place a job retires.
        activeHandleOwner_->Cancel(activeHandle_);
        return;
    }

    FinishJob(id, JobState::Cancelled,
              transport::FsError::Make(transport::FsErrorCode::Cancelled,
                                       "Cancelled before transfer started"));
}

void TransferQueue::CancelAll()
{
    // Snapshot the ids first: FinishJob mutates state and pumps the queue,
    // which must not happen while iterating the vector it can reallocate.
    std::vector<JobId> ids;
    ids.reserve(jobs_.size());
    for (const auto& job : jobs_)
        if (!job.IsTerminal()) ids.push_back(job.id);

    for (const JobId id : ids)
        CancelJob(id);
}

void TransferQueue::CancelJobsUsing(const transport::IRemoteFileSystem* fs)
{
    if (!fs) return;

    std::vector<JobId> ids;
    for (const auto& job : jobs_)
        if (!job.IsTerminal() && job.Uses(fs)) ids.push_back(job.id);

    for (const JobId id : ids) {
        // The filesystem is going away, so its handle cannot be asked to stop
        // politely; retire the job directly rather than waiting for a
        // confirmation that will never arrive.
        if (TransferJob* job = Find(id)) {
            DiscardStagingFile(*job);
            if (job->source.fs == fs)      job->source.fs = nullptr;
            if (job->destination.fs == fs) job->destination.fs = nullptr;
        }
        FinishJob(id, JobState::Cancelled,
                  transport::FsError::Make(transport::FsErrorCode::NotConnected,
                                           "The session for this transfer closed"));
    }
}

size_t TransferQueue::ClearFinished()
{
    const size_t before = jobs_.size();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const TransferJob& j) { return j.IsTerminal(); }),
                jobs_.end());
    return before - jobs_.size();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

uint64_t TransferQueue::TotalBytes() const
{
    uint64_t total = 0;
    for (const auto& job : jobs_) total += job.totalBytes;
    return total;
}

uint64_t TransferQueue::TransferredBytes() const
{
    uint64_t total = 0;
    for (const auto& job : jobs_) total += job.transferredBytes;
    return total;
}

size_t TransferQueue::PendingCount() const
{
    return static_cast<size_t>(
        std::count_if(jobs_.begin(), jobs_.end(),
                      [](const TransferJob& j) { return !j.IsTerminal(); }));
}

bool TransferQueue::IsIdle() const
{
    return activeId_ == kInvalidJobId && PendingCount() == 0;
}

} // namespace term::fs
