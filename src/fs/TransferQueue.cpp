#include "fs/TransferQueue.h"
#include "fs/RemotePath.h"

#include <algorithm>
#include <deque>
#include <filesystem>
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

bool LocalExists(const std::string& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

uint64_t LocalSize(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(size);
}

// Creates a directory and its parents. Returns an empty error on success or
// when it already exists.
transport::FsError EnsureLocalDirectory(const std::string& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec && !std::filesystem::is_directory(path))
        return transport::FsError::Make(transport::FsErrorCode::LocalIoError,
                                        "Cannot create local directory '" + path +
                                        "': " + ec.message());
    return {};
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

TransferQueue::TransferQueue(transport::IRemoteFileSystem& remote,
                             Dispatcher dispatch)
    : remote_(remote)
    , dispatch_(std::move(dispatch))
    , alive_(std::make_shared<std::atomic<bool>>(true))
{}

TransferQueue::~TransferQueue()
{
    // Callbacks still held by the transport check this before touching us.
    // Destruction must therefore happen on the owning thread, which is the
    // same thread the dispatcher posts to.
    alive_->store(false, std::memory_order_release);
}

TransferQueue::CallbackContext TransferQueue::Context()
{
    return CallbackContext{dispatch_, alive_, this};
}

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

JobId TransferQueue::EnqueueDownload(const std::string& remotePath,
                                     const std::string& localPath,
                                     uint64_t sizeHint)
{
    TransferJob job;
    job.direction  = TransferDirection::Download;
    job.sourcePath = remotePath;
    job.destPath   = localPath;
    job.totalBytes = sizeHint;
    return AddJob(std::move(job));
}

JobId TransferQueue::EnqueueUpload(const std::string& localPath,
                                   const std::string& remotePath,
                                   uint64_t sizeHint)
{
    TransferJob job;
    job.direction  = TransferDirection::Upload;
    job.sourcePath = localPath;
    job.destPath   = remotePath;
    // The local size is free to read, so an omitted hint is filled in rather
    // than leaving the aggregate denominator wrong.
    job.totalBytes = sizeHint ? sizeHint : LocalSize(localPath);
    return AddJob(std::move(job));
}

// ---------------------------------------------------------------------------
// Recursive expansion
// ---------------------------------------------------------------------------

void TransferQueue::EnqueueDownloadTree(
    const std::string& remoteDir,
    const std::string& localDir,
    std::function<void(transport::FsError)> onExpanded)
{
    struct PendingDir {
        std::string remote;
        std::string local;
        int         depth = 0;
    };

    // Heap state shared by the walk's continuations, mirroring how the SFTP
    // adapter carries its own multi-step tasks.
    struct Walk {
        std::deque<PendingDir>                  pending;
        transport::FsError                      firstError;
        std::function<void(transport::FsError)> onExpanded;
    };

    auto walk = std::make_shared<Walk>();
    walk->pending.push_back({remoteDir, localDir, 0});
    walk->onExpanded = std::move(onExpanded);

    // Declared as a shared_ptr to a std::function so each step can re-enter it
    // without the recursion being visible in the type.
    auto step = std::make_shared<std::function<void()>>();
    auto ctx  = Context();

    *step = [this, ctx, walk, step]() {
        if (walk->pending.empty()) {
            auto done = std::move(walk->onExpanded);
            if (done) done(walk->firstError);
            return;
        }

        const PendingDir dir = walk->pending.front();
        walk->pending.pop_front();

        if (transport::FsError err = EnsureLocalDirectory(dir.local); err.Failed()) {
            auto done = std::move(walk->onExpanded);
            if (done) done(std::move(err));
            return;
        }

        remote_.List(dir.remote,
            [ctx, walk, step, dir](std::vector<transport::FileInfo> entries,
                                   transport::FsError err) {
                ctx.post([ctx, walk, step, dir,
                          entries = std::move(entries),
                          err = std::move(err)]() mutable {
                    if (!ctx.Alive()) return;

                    // A directory that failed outright stops the walk; one
                    // that returned rows alongside an error contributes what
                    // it read and the error is reported at the end.
                    if (err.Failed() && entries.empty()) {
                        auto done = std::move(walk->onExpanded);
                        if (done) done(std::move(err));
                        return;
                    }
                    if (err.Failed() && walk->firstError.Ok())
                        walk->firstError = std::move(err);

                    for (const auto& e : entries) {
                        if (e.isSymlink) continue;   // never follow links
                        const std::string remoteChild = path::Join(dir.remote, e.name);
                        const std::string localChild =
                            (std::filesystem::path(dir.local) / e.name).string();

                        if (e.isDir) {
                            if (dir.depth + 1 < kMaxTreeDepth)
                                walk->pending.push_back(
                                    {remoteChild, localChild, dir.depth + 1});
                        } else {
                            ctx.self->EnqueueDownload(remoteChild, localChild, e.size);
                        }
                    }
                    (*step)();
                });
            });
    };

    (*step)();
}

void TransferQueue::EnqueueUploadTree(
    const std::string& localDir,
    const std::string& remoteDir,
    std::function<void(transport::FsError)> onExpanded)
{
    // The local side is walked synchronously — std::filesystem does not make
    // us wait — so the only asynchronous part is creating the remote
    // directories, which must complete before their files are queued.
    struct Entry {
        std::string relative;
        bool        isDir = false;
        uint64_t    size  = 0;
    };

    std::vector<Entry> entries;
    std::error_code ec;

    // recursive_directory_iterator does not descend into symlinked directories
    // unless asked, which is the behaviour we want; symlinked *files* are
    // skipped explicitly below.
    std::filesystem::recursive_directory_iterator it(localDir, ec), end;
    if (ec) {
        if (onExpanded)
            onExpanded(transport::FsError::Make(
                transport::FsErrorCode::LocalIoError,
                "Cannot read local directory '" + localDir + "': " + ec.message()));
        return;
    }

    const std::filesystem::path root(localDir);
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (it.depth() >= kMaxTreeDepth) { it.disable_recursion_pending(); continue; }

        const auto& entry = *it;
        if (entry.is_symlink()) { it.disable_recursion_pending(); continue; }

        Entry e;
        e.relative = std::filesystem::relative(entry.path(), root, ec).string();
        if (ec || e.relative.empty()) continue;
        e.isDir = entry.is_directory();
        e.size  = e.isDir ? 0 : LocalSize(entry.path().string());
        entries.push_back(std::move(e));
    }

    // Shallower directories first, so a parent always exists before its child
    // is created.
    std::stable_sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            const auto depth = [](const std::string& s) {
                return std::count(s.begin(), s.end(), '/');
            };
            if (a.isDir != b.isDir) return a.isDir;      // directories first
            return depth(a.relative) < depth(b.relative);
        });

    struct Walk {
        std::vector<Entry>                      entries;
        size_t                                  index = 0;
        std::string                             remoteRoot;
        std::string                             localRoot;
        std::function<void(transport::FsError)> onExpanded;
    };

    auto walk = std::make_shared<Walk>();
    walk->entries    = std::move(entries);
    walk->remoteRoot = remoteDir;
    walk->localRoot  = localDir;
    walk->onExpanded = std::move(onExpanded);

    auto step = std::make_shared<std::function<void()>>();
    auto ctx  = Context();

    *step = [this, ctx, walk, step]() {
        while (walk->index < walk->entries.size()) {
            const Entry& e = walk->entries[walk->index];
            const std::string remotePath = path::Join(walk->remoteRoot, e.relative);

            if (!e.isDir) {
                const std::string localPath =
                    (std::filesystem::path(walk->localRoot) / e.relative).string();
                ++walk->index;
                EnqueueUpload(localPath, remotePath, e.size);
                continue;
            }

            ++walk->index;
            remote_.MakeDirectory(remotePath, 0755,
                [ctx, step](transport::FsError err) {
                    ctx.post([ctx, step, err = std::move(err)]() mutable {
                        if (!ctx.Alive()) return;
                        // A directory that is already there is not a failure —
                        // it is the normal case when merging into an existing
                        // tree. Any other error is left for the file transfers
                        // to surface against the specific path that fails.
                        (void)err;
                        (*step)();
                    });
                });
            return;   // resume from the callback
        }

        auto done = std::move(walk->onExpanded);
        if (done) done(transport::FsError::Success());
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

    job->state = JobState::Checking;
    NotifyChanged(id);

    if (job->direction == TransferDirection::Download) {
        // The destination is local, so the answer is available immediately.
        // It is still posted rather than used inline: a run of skipped jobs
        // would otherwise recurse once per job and could exhaust the stack.
        const bool exists = LocalExists(job->destPath);
        auto ctx = Context();
        ctx.post([ctx, id, exists] {
            if (!ctx.Alive()) return;
            ctx.self->OnConflictKnown(id, exists);
        });
        return;
    }

    auto ctx = Context();
    const std::string dest = job->destPath;
    remote_.Stat(dest, [ctx, id](transport::FileInfo, transport::FsError err) {
        ctx.post([ctx, id, err = std::move(err)]() mutable {
            if (!ctx.Alive()) return;
            // Only a definitive "not there" counts as no conflict. Any other
            // error (a permission problem on the parent, say) is left to the
            // transfer itself to report against the real operation, rather
            // than being second-guessed here.
            const bool exists = !(err.code == transport::FsErrorCode::NoSuchFile);
            ctx.self->OnConflictKnown(id, exists);
        });
    });
}

void TransferQueue::OnConflictKnown(JobId id, bool exists)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }
    // Cancelled while the check was in flight.
    if (job->state != JobState::Checking) { activeId_ = kInvalidJobId; Pump(); return; }

    if (!exists) { StartTransfer(id); return; }

    switch (policy_) {
        case ConflictPolicy::Overwrite:
            StartTransfer(id);
            return;
        case ConflictPolicy::Skip:
            FinishJob(id, JobState::Skipped);
            return;
        case ConflictPolicy::Rename:
            ResolveFreeName(id, 1);
            return;
        case ConflictPolicy::Ask:
            break;
    }

    if (!prompt_) {
        // Nothing can answer, so take the only choice that cannot destroy the
        // user's data.
        FinishJob(id, JobState::Skipped);
        return;
    }

    job->state = JobState::AwaitingResolution;
    NotifyChanged(id);

    auto ctx = Context();
    prompt_(*job, [ctx, id](ConflictResolution resolution, bool applyToAll) {
        ctx.post([ctx, id, resolution, applyToAll] {
            if (!ctx.Alive()) return;
            TransferQueue& q = *ctx.self;
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
        case ConflictResolution::Overwrite: StartTransfer(id);              return;
        case ConflictResolution::Skip:      FinishJob(id, JobState::Skipped); return;
        case ConflictResolution::Rename:    ResolveFreeName(id, 1);         return;
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

    if (job->direction == TransferDirection::Download) {
        const std::filesystem::path dest(job->destPath);
        const std::string candidate =
            (dest.parent_path() /
             MakeUniqueCandidate(dest.filename().string(), attempt)).string();
        if (LocalExists(candidate)) { ResolveFreeName(id, attempt + 1); return; }
        job->destPath = candidate;
        NotifyChanged(id);
        StartTransfer(id);
        return;
    }

    const std::string candidate =
        path::Join(path::Parent(job->destPath),
                   MakeUniqueCandidate(path::Leaf(job->destPath), attempt));

    auto ctx = Context();
    remote_.Stat(candidate,
        [ctx, id, candidate, attempt](transport::FileInfo, transport::FsError err) {
            ctx.post([ctx, id, candidate, attempt, err = std::move(err)]() mutable {
                if (!ctx.Alive()) return;
                TransferQueue& q = *ctx.self;
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

    auto ctx = Context();
    const auto onProgress = [ctx, id](uint64_t done, uint64_t total) {
        ctx.post([ctx, id, done, total] {
            if (!ctx.Alive()) return;
            ctx.self->OnProgress(id, done, total);
        });
    };
    const auto onDone = [ctx, id](transport::FsError err) {
        ctx.post([ctx, id, err = std::move(err)]() mutable {
            if (!ctx.Alive()) return;
            ctx.self->OnTransferDone(id, std::move(err));
        });
    };

    activeHandle_ = (job->direction == TransferDirection::Download)
        ? remote_.Download(job->sourcePath, job->destPath, onProgress, onDone)
        : remote_.Upload(job->sourcePath, job->destPath, onProgress, onDone);
}

void TransferQueue::OnProgress(JobId id, uint64_t transferred, uint64_t total)
{
    TransferJob* job = Find(id);
    if (!job || job->state != JobState::Active) return;
    job->transferredBytes = transferred;
    // Trust the transport's figure once it has one: a size hint taken from a
    // directory listing can be stale by the time the bytes actually move.
    if (total) job->totalBytes = total;
    NotifyChanged(id);
}

void TransferQueue::OnTransferDone(JobId id, transport::FsError err)
{
    if (err.Ok()) {
        if (TransferJob* job = Find(id))
            job->transferredBytes = job->totalBytes;
        FinishJob(id, JobState::Completed);
        return;
    }
    const bool cancelled = err.code == transport::FsErrorCode::Cancelled;
    FinishJob(id, cancelled ? JobState::Cancelled : JobState::Failed, std::move(err));
}

void TransferQueue::FinishJob(JobId id, JobState state, transport::FsError err)
{
    if (TransferJob* job = Find(id)) {
        job->state = state;
        job->error = std::move(err);
        NotifyChanged(id);
    }
    if (activeId_ == id) {
        activeId_     = kInvalidJobId;
        activeHandle_ = transport::kInvalidTransferHandle;
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

    if (job->state == JobState::Active && id == activeId_) {
        // The transport owns the job now; it will report Cancelled through the
        // normal completion path so there is exactly one place a job retires.
        remote_.Cancel(activeHandle_);
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
