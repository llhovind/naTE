#include "fs/TransferQueue.h"
#include "fs/FileMode.h"
#include "fs/RelayWorkspace.h"
#include "fs/RemotePath.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <system_error>

#include <unistd.h>

namespace term::fs {

namespace {

// Guards against a pathologically deep tree, and against a server that reports
// a directory containing itself. A walk never descends into a symlink whatever
// the policy says to do with it, so cycles are already impossible by
// construction and this is a backstop rather than the primary defence.
constexpr int kMaxTreeDepth = 64;

// How many alternative names a Rename resolution will try before giving up.
// A collision that survives this many attempts means something is wrong that
// renaming will not fix.
constexpr int kMaxRenameAttempts = 100;

// How far up the destination's ancestry a space query will climb before giving
// up. Deep enough to clear any tree the walk could have just created, shallow
// enough that an endpoint answering nothing costs a handful of round trips
// rather than one per path component all the way to the root.
constexpr int kMaxSpaceProbeAncestors = 8;

// Whether a failure means the endpoint is gone rather than that one path is a
// problem. The distinction decides whether a tree walk skips and carries on or
// abandons the batch: everything else is local to the directory that produced
// it, and losing the whole selection over one of those is what this exists to
// stop.
bool IsEndpointGone(transport::FsErrorCode code) noexcept
{
    return code == transport::FsErrorCode::NotConnected ||
           code == transport::FsErrorCode::Cancelled;
}

// The bits a copy reproduces on the destination.
//
// The nine permission bits and nothing else. setuid, setgid and the sticky bit
// are deliberately dropped: cp(1) drops them for the same reason, and a copy
// that recreated a setuid binary somewhere the user merely has write access is
// a privilege escalation waiting to be noticed by someone else.
std::optional<uint32_t> PermissionsToCopy(std::optional<uint32_t> mode)
{
    if (!mode) return std::nullopt;
    return *mode & kPermissionMask;
}

// Scratch path for a transfer that has to be staged through this machine.
//
// The naming, and the pid inside it, belong to RelayWorkspace: a staging file
// abandoned by a killed process has to be reclaimable by the next run, and only
// something that knows the whole scheme can tell one of those from a file
// another live instance is relaying through right now.
std::string MakeStagingPath(JobId id, const std::string& leaf)
{
    const auto stamp = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return MakeRelayPath(leaf, id, ::getpid(), stamp);
}

} // namespace

TransferItem ItemForLocalPath(const std::string& path)
{
    TransferItem item;
    item.path = path;
    item.name = std::filesystem::path(path).filename().string();

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) return item;   // unreadable: reported as a plain file and left to fail visibly

    item.isSymlink = std::filesystem::is_symlink(status);
    item.isDir     = std::filesystem::is_directory(status);
    // The link's own permissions are meaningless, and a link is reproduced
    // rather than copied, so only a real file contributes a mode.
    if (!item.isSymlink)
        item.mode = static_cast<uint32_t>(status.permissions()) & kPermissionMask;

    if (!item.isDir && !item.isSymlink) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) item.size = static_cast<uint64_t>(size);
    }
    return item;
}

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

TransferQueue::TransferQueue(Dispatcher dispatch, StagingSpaceProbe stagingSpace)
    : guard_(std::move(dispatch)),
      stagingSpace_(stagingSpace ? std::move(stagingSpace)
                                 : StagingSpaceProbe(&RelayVolumeSpace))
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
                             SourceAttributes attributes)
{
    TransferJob job;
    job.source      = std::move(source);
    job.destination = std::move(destination);
    job.sourcePath  = sourcePath;
    job.destPath    = destPath;
    job.totalBytes  = attributes.size;
    job.sourceMode  = PermissionsToCopy(attributes.mode);

    // Neither side is this machine, so the bytes must come down and go back
    // up. They travel twice, and the denominator says so rather than leaving
    // a progress bar apparently stuck halfway.
    job.viaLocalStaging = job.source.Valid() && job.destination.Valid() &&
                          !job.source.IsLocalDisk() && !job.destination.IsLocalDisk();
    if (job.viaLocalStaging) job.totalBytes = attributes.size * 2;

    return AddJob(std::move(job));
}

void TransferQueue::EnqueueItem(TransferEndpoint source, const TransferItem& item,
                                TransferEndpoint destination,
                                const std::string& destinationDir,
                                std::function<void(transport::FsError)> onExpanded)
{
    const std::string dest = path::Join(destinationDir, item.name);

    // A link is decided by policy before anything else is considered — whether
    // it points at a directory is not this code's business under Preserve or
    // Skip, and asking would cost a round trip to answer a question nobody has.
    if (item.isSymlink) {
        EnqueueSymlink(std::move(source), item, std::move(destination), dest);
        if (onExpanded) onExpanded(transport::FsError::Success());
        return;
    }

    if (item.isDir) {
        EnqueueTree(std::move(source), item.path, std::move(destination), dest,
                    std::move(onExpanded));
        return;
    }

    Enqueue(std::move(source), item.path, std::move(destination), dest,
            {item.size, item.mode});
    // Nothing to expand, but the caller is told either way so it does not have
    // to know which shape it queued.
    if (onExpanded) onExpanded(transport::FsError::Success());
}

void TransferQueue::EnqueueSymlink(TransferEndpoint source, const TransferItem& item,
                                   TransferEndpoint destination,
                                   const std::string& destPath)
{
    TransferJob job;
    job.kind        = JobKind::Link;
    job.source      = std::move(source);
    job.destination = std::move(destination);
    job.sourcePath  = item.path;
    job.destPath    = destPath;

    if (symlinks_ == SymlinkPolicy::Skip) {
        // Recorded rather than dropped. A copy that silently omits things is
        // indistinguishable from one that lost them.
        job.state = JobState::Skipped;
        job.error = transport::FsError::Make(
            transport::FsErrorCode::Cancelled,
            "Skipped: links are not being copied");
        AddJob(std::move(job));
        return;
    }

    // Preserve. Follow is reserved and unreachable — no UI offers it and the
    // config parser will not produce it — so it is deliberately not a case
    // here rather than being quietly treated as one of the others.
    AddJob(std::move(job));
}

void TransferQueue::EnqueueTree(TransferEndpoint source, const std::string& sourceDir,
                                TransferEndpoint destination, const std::string& destDir,
                                std::function<void(transport::FsError)> onExpanded)
{
    // A directory copied into itself makes the walk chase the copies it is
    // creating: every listing turns up the directory written a moment earlier,
    // and only the depth cap ends it — having filled the tree with dozens of
    // nested duplicates first. cp(1) refuses this for the same reason. Only
    // meaningful within one filesystem; the identical path on two machines is
    // two different places.
    if (source.fs == destination.fs && path::Contains(sourceDir, destDir)) {
        if (onExpanded)
            onExpanded(transport::FsError::Make(
                transport::FsErrorCode::InvalidName,
                "Cannot copy '" + sourceDir + "' into itself"));
        return;
    }

    // Held from here until the walk reports, so nothing starts moving before
    // the batch is fully known. Released through the walk's own completion
    // hook, which fires exactly once on every path it can take — including the
    // ones that stop early on an error.
    toVisit_.push_back({std::move(source), std::move(destination),
                        sourceDir, destDir, 0});
    // The root of the tree is a directory the copy has to make, exactly like
    // the ones found inside it.
    dirsToCreate_.push_back({toVisit_.back().destination, destDir});

    if (onExpanded) enumCallbacks_.push_back(std::move(onExpanded));
    if (!enumerating_) {
        enumerating_ = true;
        enumError_   = {};
        StepEnumeration();
    }
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------
//
// One directory at a time, for the whole queue, and nothing is written.
//
// Both properties are deliberate and both were learned the hard way. Several
// trees used to be walked at once, each with its own continuation chain, which
// meant several listings in flight on one SFTP session — and libssh2 keeps
// readdir state per session, so two overlapping listings trade answers and a
// directory's contents arrive attached to another directory's path. A single
// queue with a single listing outstanding cannot express that bug. It is slower
// on a wide tree, and that is the right trade: an enumeration that takes longer
// is worth any amount more than one that is subtly wrong.
//
// Nothing is written because enumeration happens before the user has agreed to
// anything. Creating the destination skeleton as the walk discovered it left
// directories behind on a copy the user then declined, which is not something a
// preview may do.

void TransferQueue::StepEnumeration()
{
    if (toVisit_.empty()) {
        FinishEnumeration();
        return;
    }

    const PendingDir dir = toVisit_.front();
    toVisit_.pop_front();

    auto ctx = guard_.For(this);
    dir.source.fs->List(dir.sourcePath,
        [ctx, dir](std::vector<transport::FileInfo> entries,
                   transport::FsError err) {
            ctx.Post([dir, entries = std::move(entries),
                      err = std::move(err)](TransferQueue& q) mutable {
                // A batch cancelled while this listing was in flight must not
                // have its results absorbed: the jobs and directories they
                // would add are precisely what the cancel just took away, and
                // absorbing them would refill a queue the user emptied.
                if (!q.enumCancelled_)
                    q.AbsorbListing(dir, entries, std::move(err));
                q.StepEnumeration();
            });
        });
}

void TransferQueue::AbsorbListing(const PendingDir& dir,
                                  const std::vector<transport::FileInfo>& entries,
                                  transport::FsError err)
{
    if (err.Failed()) {
        // A directory that could not be read costs that directory, not the rest
        // of the tree — except when the endpoint itself is gone, where every
        // remaining listing would fail identically and pressing on would spend
        // a doomed round trip per directory to reach the same place.
        if (IsEndpointGone(err.code)) toVisit_.clear();

        NoteUnreadableDirectory(dir.sourcePath, err);
        if (enumError_.Ok()) enumError_ = std::move(err);
        // A listing that returned rows alongside an error still contributes
        // what it read; discarding those would lose useful work.
        if (entries.empty()) return;
    }

    for (const auto& e : entries) {
        const std::string src = path::Join(dir.sourcePath, e.name);
        const std::string dst = path::Join(dir.destPath, e.name);

        // A link inside the tree gets the same treatment as one the user picked
        // directly, and is never descended into — which is what keeps the walk
        // free of cycles by construction.
        if (e.isSymlink) {
            TransferItem link;
            link.path      = src;
            link.name      = e.name;
            link.isSymlink = true;
            link.isDir     = e.isDir;
            EnqueueSymlink(dir.source, link, dir.destination, dst);
            continue;
        }

        if (e.isDir) {
            if (dir.depth + 1 >= kMaxTreeDepth) continue;
            toVisit_.push_back({dir.source, dir.destination, src, dst,
                                dir.depth + 1});
            // Discovery order is breadth-first, so a parent is always recorded
            // before its children and creating them in this order needs no
            // sorting and no recursive mkdir.
            dirsToCreate_.push_back({dir.destination, dst});
        } else {
            Enqueue(dir.source, src, dir.destination, dst, {e.size, e.mode});
        }
    }
}

void TransferQueue::FinishEnumeration()
{
    enumerating_   = false;
    enumCancelled_ = false;

    // Handed back before the queue acts on the batch: the callers are reporting
    // on the enumeration, and one of them may still cancel what it produced.
    auto callbacks = std::move(enumCallbacks_);
    enumCallbacks_.clear();
    for (auto& done : callbacks)
        if (done) done(enumError_);

    Pump();
}

void TransferQueue::CreateNextDirectory()
{
    const PendingDirCreate next = dirsToCreate_.front();
    dirsToCreate_.pop_front();

    auto ctx = guard_.For(this);
    // The error is deliberately ignored: merging into a tree that already
    // exists is the ordinary case, and a directory that genuinely cannot be
    // made will fail again, visibly, on the first file that belongs in it.
    next.destination.fs->MakeDirectory(next.path, kDefaultDirectoryMode,
        [ctx](transport::FsError) {
            ctx.Post([](TransferQueue& q) { q.Pump(); });
        });
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
    if (enumerating_) return;   // still working out what the batch contains

    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
        [](const TransferJob& j) { return j.state == JobState::Queued; });
    if (it == jobs_.end()) {
        // Not idle while something is still moving: the last job of a batch is
        // no longer queued long before it has finished.
        if (activeId_ != kInvalidJobId) return;

        // An empty directory is still a directory the user asked to copy, and
        // it has no file job to carry it.
        if (!dirsToCreate_.empty() && !paused_) {
            CreateNextDirectory();
            return;
        }

        // The one way a tally is raised that no verdict ever collects: a walk
        // where every directory was unreadable queues nothing, so there is
        // never a batch for a report to be built about.
        ResetEnumerationTally();
        if (listener_) listener_->OnTransferQueueIdle();
        return;
    }

    // Work no verdict has covered — either queued after the last one, or bound
    // for a volume that verdict did not measure. Asked of every queued job
    // rather than just the next one, because the case this exists for is a
    // second copy started while the first is still draining: the job at the
    // front is then old, cleared work, and waiting for it to be reached would
    // hold the warning back until the bytes were nearly moving.
    if (preflight_ == Preflight::Cleared && FirstUnchecked())
        preflight_ = Preflight::Pending;

    // Deliberately ahead of the one-at-a-time rule: the check costs a round
    // trip and does not touch the transfer in flight, so it runs alongside it.
    // The user hears that a batch will not fit when they ask for it, rather
    // than whenever the queue eventually works its way round to it.
    if (preflight_ != Preflight::Cleared) {
        // Checking means a query or the user's answer is outstanding, and
        // whichever it is will pump again when it lands.
        if (preflight_ == Preflight::Pending) BeginPreflight();
        return;
    }

    // After the check, not before it: a paused queue that is about to be
    // resumed should already know whether the batch fits, and the check costs
    // nothing while nothing is moving.
    if (paused_) return;
    if (activeId_ != kInvalidJobId) return;   // one at a time

    // After the check and after the user's answer, never before: this is the
    // first point at which the copy has been agreed to, and it is the first
    // point at which anything may be written to the destination.
    if (!dirsToCreate_.empty()) {
        CreateNextDirectory();
        return;
    }

    activeId_ = it->id;
    BeginJob(activeId_);
}

void TransferQueue::Pause()
{
    paused_ = true;
    // Nothing to stop: the transfer in flight is deliberately left to finish.
    // Listeners still hear about it, because a queue that has stopped starting
    // work looks different to a user than one that is merely slow.
    if (listener_ && activeId_ != kInvalidJobId) NotifyChanged(activeId_);
}

void TransferQueue::Resume()
{
    if (!paused_) return;
    paused_ = false;
    Pump();
}

// ---------------------------------------------------------------------------
// Pre-flight
// ---------------------------------------------------------------------------

TransferJob* TransferQueue::FirstUnchecked()
{
    const auto it = std::find_if(jobs_.begin(), jobs_.end(),
        [](const TransferJob& j) {
            return j.state == JobState::Queued && !j.spaceChecked;
        });
    return it == jobs_.end() ? nullptr : &*it;
}

void TransferQueue::BeginPreflight()
{
    preflight_ = Preflight::Checking;

    TransferJob* job = FirstUnchecked();
    if (!job) {
        ClearPreflight();
        return;
    }
    if (!job->destination.Valid()) {
        // Nothing to measure against. Marked weighed rather than left standing:
        // an invalid endpoint is BeginJob's to report, and a job that can
        // neither be checked nor retired here would have Pump come straight
        // back for it. Clearing rather than holding, for the same reason — a
        // batch stuck behind a check that can never run reports nothing at all.
        job->spaceChecked = true;
        ClearPreflight();
        return;
    }

    // The volume is the destination *directory's*, not the file's: the file is
    // what is about to be created and does not exist yet. A host commonly has
    // several volumes, so asking about the wrong path gives a confident answer
    // about the wrong disk.
    QuerySpaceFrom(job->destination.fs, path::Parent(job->destPath),
                   kMaxSpaceProbeAncestors);
}

void TransferQueue::QuerySpaceFrom(transport::IRemoteFileSystem* fs,
                                   std::string path, int ancestorsLeft)
{
    if (!fs || path.empty()) {
        OnPreflightSpace(std::nullopt);
        return;
    }

    auto ctx = guard_.For(this);
    fs->QuerySpace(path, [ctx, fs, path, ancestorsLeft](
                             transport::FsSpaceInfo info,
                             transport::FsError err) {
        ctx.Post([fs, path, ancestorsLeft, info,
                  err = std::move(err)](TransferQueue& q) mutable {
            if (err.Ok()) {
                q.OnPreflightSpace(info);
                return;
            }

            // The server has no statvfs at all, so climbing is pointless.
            const std::string parent = path::Parent(path);
            if (err.code == transport::FsErrorCode::Unsupported ||
                ancestorsLeft <= 0 || parent.empty() || parent == path) {
                // Unmeasured — never zero. See FsSpaceInfo.
                q.OnPreflightSpace(std::nullopt);
                return;
            }
            q.QuerySpaceFrom(fs, parent, ancestorsLeft - 1);
        });
    });
}

void TransferQueue::OnPreflightSpace(std::optional<transport::FsSpaceInfo> destination)
{
    // The volume this verdict is about: whichever one BeginPreflight asked
    // about, re-derived the same way so the figure measured and the bytes
    // totalled cannot describe different destinations.
    const TransferJob* first = FirstUnchecked();
    if (!first) {
        // Everything the query was issued for has since been cancelled or
        // started. Nothing left to weigh, and nothing to say about it.
        ClearPreflight();
        return;
    }
    const transport::IRemoteFileSystem* const volume = first->destination.fs;

    // Gathered now rather than when the query was issued: a view queueing a
    // multi-file selection is still adding to the batch while the query is in
    // flight, and the callback is dispatched, so by here the loop has run.
    // Jobs bound for a different filesystem are left out *and left unchecked* —
    // they are not on the volume that was measured, counting them against it
    // would produce a shortfall out of files that are not going there, and Pump
    // will weigh them against their own volume on the next turn.
    std::vector<SpaceDemand> demands;
    bool anyStaged = false;

    for (auto& job : jobs_) {
        if (job.state != JobState::Queued) continue;
        if (job.destination.fs != volume) continue;
        // Weighed, whether or not it contributes bytes. A link occupies no
        // space worth counting but must still be marked, or a batch of nothing
        // but links would leave the queue asking about them forever.
        job.spaceChecked = true;
        if (job.kind != JobKind::Copy) continue;

        // totalBytes counts a staged transfer's bytes twice, once per leg. What
        // lands on the destination is one copy of the file.
        const uint64_t bytes =
            job.viaLocalStaging ? job.totalBytes / 2 : job.totalBytes;

        demands.push_back({bytes, job.viaLocalStaging});
        anyStaged = anyStaged || job.viaLocalStaging;
    }

    // Only asked for when something in the batch actually stages through it, so
    // an ordinary upload does not stat a volume it will never touch.
    std::optional<transport::FsSpaceInfo> staging;
    if (anyStaged && stagingSpace_) staging = stagingSpace_();

    PreflightReport report;
    report.forecast              = ForecastSpace(demands, destination, staging);
    report.unreadableDirectories = unreadableDirs_;
    report.firstFailure          = firstUnreadable_;
    report.firstFailurePath      = firstUnreadablePath_;
    lastPreflight_               = std::move(report);
    // Consumed by the report now carrying it. A queue holding work for two
    // destinations reaches a verdict per volume, and the second must not repeat
    // the first's skipped directories as though it had found them itself.
    ResetEnumerationTally();

    if (!lastPreflight_->Concerning() || !spacePrompt_) {
        ClearPreflight();
        return;
    }

    auto ctx = guard_.For(this);
    spacePrompt_(*lastPreflight_, [ctx, volume](bool proceed) {
        ctx.Post([proceed, volume](TransferQueue& q) {
            // Declining abandons the work this verdict covered rather than
            // merely leaving it parked: a queue held indefinitely behind an
            // answered prompt looks to the user exactly like one that has hung.
            //
            // Only that work, though. The warning named one destination and
            // totalled one volume's worth of files, so "no" is an answer about
            // those — a copy queued in the other direction was never what was
            // being asked about and is not this answer's to throw away.
            if (!proceed) q.CancelBatchFor(volume);

            // Either way the check is over and must be recorded as over. Pump
            // refuses to start anything while one is outstanding, so a queue
            // left in Checking never moves another byte: before this, declining
            // a single warning wedged the window's queue for good, and every
            // copy asked for afterwards sat waiting on an answer already given.
            q.ClearPreflight();
        });
    });
}

void TransferQueue::NoteUnreadableDirectory(const std::string& path,
                                            const transport::FsError& err)
{
    ++unreadableDirs_;
    // The first is kept rather than the last: it is the one nearest the top of
    // the tree, and on a selection where a whole subtree is unreachable every
    // failure below it says the same thing less usefully.
    if (firstUnreadablePath_.empty()) {
        firstUnreadable_     = err;
        firstUnreadablePath_ = path;
    }
}

void TransferQueue::ResetEnumerationTally()
{
    unreadableDirs_ = 0;
    firstUnreadable_ = {};
    firstUnreadablePath_.clear();
}

void TransferQueue::ClearPreflight()
{
    // What the verdict covers is recorded on the jobs themselves, so there is
    // nothing to record here. Pump decides what to do next: run the cleared
    // work, or weigh whatever this verdict did not reach.
    preflight_ = Preflight::Cleared;
    Pump();
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

    job->destExisted = true;

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
                    // The name it settled on is free by construction, so
                    // nothing is being replaced any more.
                    j->destExisted = false;
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

    if (job->kind == JobKind::Link) { StartLink(id); return; }

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
                                                  job->sourceMode,
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
                                                     job->sourceMode,
                                                     onProgress, onDone));
        return;
    }

    // Server to server: pull it down first. SFTP has no server-to-server copy,
    // so the bytes pass through this machine whether or not the user thinks of
    // it that way. The staging file is created with the source's permissions
    // too — a file the user keeps at 0600 must not sit world-readable in /tmp
    // on the way past.
    if (!EnsureRelayRoot()) {
        FinishJob(id, JobState::Failed,
                  transport::FsError::Make(
                      transport::FsErrorCode::LocalIoError,
                      "Cannot create the staging directory '" + RelayRoot() + "'"));
        return;
    }

    job->tempPath = MakeStagingPath(id, path::Leaf(job->sourcePath));
    const auto onDone = [ctx, id](transport::FsError err) {
        ctx.Post([id, err = std::move(err)](TransferQueue& q) mutable {
            q.OnLegDone(id, std::move(err), true);
        });
    };
    activeHandleOwner_ = job->source.fs;
    RecordHandle(id, job->source.fs->Download(job->sourcePath, job->tempPath,
                                              job->sourceMode,
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

void TransferQueue::StartLink(JobId id)
{
    TransferJob* job = Find(id);
    if (!job) { activeId_ = kInvalidJobId; Pump(); return; }

    const std::string destPath   = job->destPath;
    const bool        mustUnlink = job->destExisted;

    // Read the target from the source, then write the same text at the
    // destination. The target is never resolved: reproducing a link means
    // reproducing where it points, not where it currently lands.
    auto ctx = guard_.For(this);
    job->source.fs->ReadLink(job->sourcePath,
        [ctx, id, destPath, mustUnlink](std::string target, transport::FsError err) {
            ctx.Post([id, destPath, mustUnlink, target = std::move(target),
                      err = std::move(err)](TransferQueue& q) mutable {
                if (err.Failed()) { q.FinishJob(id, JobState::Failed, std::move(err)); return; }

                TransferJob* j = q.Find(id);
                if (!j) { q.activeId_ = kInvalidJobId; q.Pump(); return; }

                auto write = [id, destPath, target](TransferQueue& qq) {
                    TransferJob* jj = qq.Find(id);
                    if (!jj) { qq.activeId_ = kInvalidJobId; qq.Pump(); return; }

                    auto inner = qq.guard_.For(&qq);
                    jj->destination.fs->CreateSymlink(target, destPath,
                        [inner, id](transport::FsError linkErr) {
                            inner.Post([id, linkErr = std::move(linkErr)](
                                           TransferQueue& q3) mutable {
                                if (linkErr.Failed()) {
                                    q3.FinishJob(id, JobState::Failed, std::move(linkErr));
                                    return;
                                }
                                q3.FinishJob(id, JobState::Completed);
                            });
                        });
                };

                if (!mustUnlink) { write(q); return; }

                // Overwriting: a symlink cannot be created over an existing
                // path, so the old entry goes first. Its removal failing is
                // this job's failure — the link would fail next anyway, and
                // reporting the cause is more use than reporting the symptom.
                auto inner = q.guard_.For(&q);
                j->destination.fs->Remove(destPath, false,
                    [inner, id, write](transport::FsError rmErr) {
                        inner.Post([id, write, rmErr = std::move(rmErr)](
                                       TransferQueue& q2) mutable {
                            if (rmErr.Failed()) {
                                q2.FinishJob(id, JobState::Failed, std::move(rmErr));
                                return;
                            }
                            write(q2);
                        });
                    });
            });
        });
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
    // The mode carried here is the *source's*, not the staging file's: the
    // staging file is an implementation detail, and its permissions must not
    // become the ones the user's copy ends up with.
    RecordHandle(id, job->destination.fs->Upload(job->tempPath, job->destPath,
                                                 job->sourceMode,
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
        cancellingActive_  = false;
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
        // Until it does, the queue is neither running nor stopped, and says so.
        cancellingActive_ = true;
        activeHandleOwner_->Cancel(activeHandle_);
        NotifyChanged(id);
        return;
    }

    FinishJob(id, JobState::Cancelled,
              transport::FsError::Make(transport::FsErrorCode::Cancelled,
                                       "Cancelled before transfer started"));
}

void TransferQueue::CancelAll()
{
    // Anything not yet listed, and every directory the copy had not yet made.
    //
    // Declining is the main way this is reached, and a declined copy must leave
    // the destination exactly as it found it. Dropping the jobs while leaving
    // the directory list standing would have Pump quietly build the skeleton of
    // a transfer the user just refused.
    toVisit_.clear();
    dirsToCreate_.clear();
    // A listing may already be at the server. Its answer cannot be unasked, but
    // it can be ignored when it lands.
    if (enumerating_) enumCancelled_ = true;

    // Snapshot the ids first: FinishJob mutates state and pumps the queue,
    // which must not happen while iterating the vector it can reallocate.
    std::vector<JobId> ids;
    ids.reserve(jobs_.size());
    for (const auto& job : jobs_)
        if (!job.IsTerminal()) ids.push_back(job.id);

    for (const JobId id : ids)
        CancelJob(id);
}

void TransferQueue::CancelBatchFor(const transport::IRemoteFileSystem* volume)
{
    // The directories the copy had not yet made *there*. A declined copy must
    // leave its destination exactly as it found it, and dropping the jobs while
    // leaving the directory list standing would have Pump quietly build the
    // skeleton of a transfer the user just refused. Another volume's pending
    // directories belong to a batch this answer was not about.
    std::deque<PendingDirCreate> keepDirs;
    for (auto& dir : dirsToCreate_)
        if (dir.destination.fs != volume) keepDirs.push_back(std::move(dir));
    dirsToCreate_.swap(keepDirs);

    // Directories still waiting to be listed for it. A listing already at the
    // server needs no equivalent: Pump will not begin a check while a walk is
    // running, so there is never one in flight at the moment this is reached —
    // which is as well, since a single outstanding listing could not be
    // discarded on one volume's behalf without discarding the other's too.
    std::deque<PendingDir> keepVisits;
    for (auto& dir : toVisit_)
        if (dir.destination.fs != volume) keepVisits.push_back(std::move(dir));
    toVisit_.swap(keepVisits);

    // Snapshot the ids first: FinishJob mutates state and pumps the queue,
    // which must not happen while iterating the vector it can reallocate.
    std::vector<JobId> ids;
    for (const auto& job : jobs_)
        if (!job.IsTerminal() && job.destination.fs == volume) ids.push_back(job.id);

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
