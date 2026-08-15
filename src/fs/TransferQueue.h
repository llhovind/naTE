#pragma once
#include "fs/Dispatcher.h"
#include "fs/PreflightReport.h"
#include "fs/SpaceForecast.h"
#include "fs/SymlinkPolicy.h"
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
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

// One thing the user picked to copy.
//
// Carries the full path, which transport::FileInfo deliberately does not, plus
// the three attributes routing turns on — so deciding what a selection means
// costs no extra round trip.
struct TransferItem {
    std::string path;
    std::string name;
    // Link-not-target, matching what both adapters' listings report: a symlink
    // to a directory has isSymlink true and isDir false. These describe what
    // the path *is*; what to do about it is SymlinkPolicy's business, and
    // keeping that separation is why the two used to disagree.
    bool        isDir     = false;
    bool        isSymlink = false;
    uint64_t    size      = 0;
    // Raw POSIX mode as the listing reported it, file-type bits included.
    // Empty when whatever produced the item could not read it, which is the
    // only honest way to say "do not reproduce permissions you never saw".
    std::optional<uint32_t> mode;
};

// Describes a path on the machine naTE runs on, for callers that have a path
// but no listing to read it from — a desktop drag-and-drop, most obviously.
//
// Uses symlink_status(), not status(), so a link is reported as a link exactly
// as a directory listing would report it. std::filesystem::is_directory follows
// links and is the wrong question here; asking it was how the drop path and the
// pane came to describe the same file differently.
TransferItem ItemForLocalPath(const std::string& path);

// What a listing already told us about a source file, so a transfer need not
// ask the filesystem again for something it has just been handed.
//
// The two travel together because they are read together and are both wanted
// before a byte moves: the size sets the progress denominator, and the mode
// decides what permissions the destination is created with.
struct SourceAttributes {
    uint64_t                size = 0;
    // Permission bits only; the caller has already dropped any file-type bits.
    std::optional<uint32_t> mode;
};

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

// What a job moves. Both kinds queue, collide and cancel identically; they
// differ only in what happens once the destination is clear.
enum class JobKind {
    Copy,   // bytes
    Link,   // a symlink reproduced at the destination
};

struct TransferJob {
    JobId   id   = kInvalidJobId;
    JobKind kind = JobKind::Copy;

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

    // True when the conflict check found something at destPath and the policy
    // said to overwrite it. Only a link job acts on this: a file upload
    // truncates on open, but creating a symlink over an existing path fails,
    // so the old one has to go first — and only then, because an unconditional
    // removal would cost a round trip on every link in a tree.
    bool destExisted = false;

    // Permission bits to give the destination where the copy creates it, taken
    // from the source. Empty when the source's mode was not known.
    std::optional<uint32_t> sourceMode;

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

// Warns that a batch may not fit, and asks whether to go ahead anyway. The
// queue calls this before the first byte moves and holds the batch until the
// continuation runs; proceed=false cancels it.
//
// It asks rather than refuses because none of the figures behind it are
// authoritative — see FsSpaceInfo. A queue that declined work on the strength
// of a number that cannot see quotas, compression or thin provisioning would be
// wrong often enough to be worth disabling, and a warning the user can overrule
// is worth more than a gate they learn to route around.
using SpaceWarningPrompt =
    std::function<void(const PreflightReport&, std::function<void(bool proceed)>)>;

// Reads the space available on this machine's staging volume, or nullopt when
// it cannot be read.
//
// Injected because the staging area is a real directory on a real disk and
// there is no port between here and it: the destination's space arrives through
// IRemoteFileSystem like everything else, but the staging volume is always this
// machine, and inventing an endpoint for it would be ceremony rather than
// decoupling. A test supplies its own to describe a volume that does not exist.
using StagingSpaceProbe =
    std::function<std::optional<transport::FsSpaceInfo>()>;

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
    // An empty probe means the real staging volume, which is what production
    // wants and what makes the parameter invisible to every caller but a test.
    explicit TransferQueue(Dispatcher dispatch, StagingSpaceProbe stagingSpace = {});
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

    // Without one installed, a batch that does not fit proceeds anyway — the
    // opposite of the conflict prompt's default, and deliberately so. An
    // unanswerable conflict risks destroying a file that already exists, so the
    // safe answer there is to do nothing; an unanswerable space warning risks
    // only a transfer that fails on ENOSPC and can be retried, whereas silently
    // refusing work the user asked for is not recoverable by anything they can
    // see. When in doubt, do what was asked.
    void SetSpaceWarningPrompt(SpaceWarningPrompt prompt)
    {
        spacePrompt_ = std::move(prompt);
    }

    // The most recent pre-flight result, or nullopt before the first batch of
    // this queue has been checked. Query rather than push, so a view reads what
    // is true when it paints.
    const std::optional<PreflightReport>& LastPreflight() const noexcept
    {
        return lastPreflight_;
    }

    // Records a directory the walk could not read and skipped. Called by the
    // tree walk; public only because the walk is free-standing rather than a
    // member, which is what keeps its continuation chain readable.
    void NoteUnreadableDirectory(const std::string& path,
                                 const transport::FsError& err);

    // True while a directory walk is still working out what a batch contains.
    //
    // Worth exposing because during it the queue looks stopped from outside —
    // nothing is queued, nothing is moving — and on a large tree that can last
    // a long time. A view that cannot say "still counting" leaves the user
    // watching an apparently idle window.
    bool IsExpanding() const noexcept { return enumerating_; }

    // How copies treat symbolic links. Applies to work queued from here on;
    // jobs already queued carry the decision that was in force when they were
    // created, so changing this never rewrites what the user already asked for.
    void SetSymlinkPolicy(SymlinkPolicy policy) { symlinks_ = policy; }
    SymlinkPolicy Symlinks() const noexcept { return symlinks_; }

    // --- Enqueueing ----------------------------------------------------------
    // Transfers begin immediately — there is no separate start step.
    JobId Enqueue(TransferEndpoint source, const std::string& sourcePath,
                  TransferEndpoint destination, const std::string& destPath,
                  SourceAttributes attributes = {});

    // Queues one picked item into destinationDir, deciding for itself whether
    // that means a single transfer or a recursive walk. This is the entry point
    // a view should use: what a selection *means* is a policy, and one that
    // gets copying symlinks wrong produces dangling links on the far side.
    //
    // A symlink is copied as whatever it points at rather than reproduced as a
    // link — the target may not exist on the other side, and a dangling link is
    // a worse outcome than a real file. That is why a symlink to a directory
    // takes the single-file path rather than being walked.
    //
    // onExpanded fires once either way: immediately for a file, after the walk
    // for a directory. The transfers it queued may still be running.
    void EnqueueItem(TransferEndpoint source, const TransferItem& item,
                     TransferEndpoint destination,
                     const std::string& destinationDir,
                     std::function<void(transport::FsError)> onExpanded = {});

    // Recursively expands a directory into leaf file jobs, creating the
    // destination directories as it goes. onExpanded reports when the walk
    // finished; the transfers it queued may still be running.
    //
    // Links encountered on the way are handed to the standing SymlinkPolicy,
    // exactly as one the user picked directly would be. What no policy changes
    // is that the walk never descends *through* a link: that is what removes
    // the possibility of a cycle by construction, and it is why a link to "/"
    // cannot silently turn into a copy of the whole filesystem.
    void EnqueueTree(TransferEndpoint source, const std::string& sourceDir,
                     TransferEndpoint destination, const std::string& destDir,
                     std::function<void(transport::FsError)> onExpanded);

    // --- Control -------------------------------------------------------------
    // Stops the queue starting anything further. The transfer already in
    // flight runs to completion.
    //
    // That is the honest meaning of pause here, and the reason it is worth
    // saying out loud: SFTP has no suspend, and this queue has no resume — a
    // part-written destination file is not something a later run can pick up.
    // Stopping mid-file would therefore mean discarding the bytes already
    // moved, which is cancelling with a gentler name. Letting the current file
    // land costs at most one file and leaves the queue in a state the user can
    // actually continue from.
    //
    // Idempotent, and safe to call whether or not anything is running.
    void Pause();
    // Resumes. Anything queued, including work added while paused, is checked
    // for space as usual before it starts.
    void Resume();
    bool IsPaused() const noexcept { return paused_; }

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
    // One directory waiting to be listed, carrying its own endpoints so a
    // single queue can serve trees bound for different places.
    struct PendingDir {
        TransferEndpoint source;
        TransferEndpoint destination;
        std::string      sourcePath;
        std::string      destPath;
        int              depth = 0;
    };

    // One directory the copy must make before anything can land in it.
    struct PendingDirCreate {
        TransferEndpoint destination;
        std::string      path;
    };

    void Pump();

    // --- Enumeration ---------------------------------------------------------
    // Lists the next directory, or finishes when there are none left. Exactly
    // one listing is ever outstanding.
    void StepEnumeration();
    // Turns one listing into file jobs, links, and further directories to visit.
    void AbsorbListing(const PendingDir& dir,
                       const std::vector<transport::FileInfo>& entries,
                       transport::FsError err);
    void FinishEnumeration();
    // Makes the next destination directory. Called from Pump once the batch has
    // been checked and approved, never before.
    void CreateNextDirectory();

    // --- Pre-flight ----------------------------------------------------------
    // Where a batch stands with respect to the space check that guards it.
    enum class Preflight {
        Pending,   // queued work has not been checked yet
        Checking,  // a query or the user's answer is outstanding
        Cleared,   // this batch may run
    };

    // Asks the destination how much room it has. Called once per batch, from
    // Pump, when there is queued work and no expansion is still assembling it.
    void BeginPreflight();
    // Asks fs about path, walking up to its parent when the answer is "no such
    // path" and trying again.
    //
    // The path a batch is bound for is a directory the walk created moments
    // earlier, and a query that lands before it exists — or on a branch whose
    // creation quietly failed — would otherwise report the whole destination
    // as unmeasurable. An ancestor is on the same volume by definition, so
    // climbing costs a round trip and loses no accuracy.
    //
    // Stops immediately on Unsupported: that is the server saying it has no
    // statvfs at all, and no ancestor will answer any better.
    void QuerySpaceFrom(transport::IRemoteFileSystem* fs, std::string path,
                        int ancestorsLeft);
    // Totals the batch against what came back and decides whether to warn.
    // The batch is gathered here rather than when the query was issued: an
    // enqueue loop is still adding to it while the query is in flight, and a
    // forecast for the first item alone would understate every multi-file copy.
    void OnPreflightSpace(std::optional<transport::FsSpaceInfo> destination);
    // Releases the batch to run.
    void ClearPreflight();
    // Forgets what the previous batch's walk could not read.
    void ResetEnumerationTally();

    void BeginJob(JobId id);
    // Queues a symlink according to the standing policy: reproduced, or
    // recorded as skipped so a copy never quietly omits one.
    void EnqueueSymlink(TransferEndpoint source, const TransferItem& item,
                        TransferEndpoint destination, const std::string& destPath);
    // Reads the link's target, then writes the same link at the destination.
    void StartLink(JobId id);
    void OnConflictKnown(JobId id, bool exists);
    void ApplyResolution(JobId id, ConflictResolution resolution);
    void ResolveFreeName(JobId id, int attempt);
    void StartTransfer(JobId id);
    // Second leg of a staged remote-to-remote transfer.
    void StartUploadLeg(JobId id);
    // Records the handle an adapter returned, unless the job it belongs to has
    // already retired.
    void RecordHandle(JobId id, transport::TransferHandle handle);
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
    SpaceWarningPrompt      spacePrompt_;
    StagingSpaceProbe       stagingSpace_;
    ITransferQueueListener* listener_ = nullptr;

    // --- Enumeration ---------------------------------------------------------
    // One directory outstanding at a time, for the whole queue. See the
    // Enumeration section in the .cpp for why it is deliberately this dull.
    //
    // Nothing starts while it runs: the walk queues its leaves over many turns,
    // and a space check that ran before it finished would forecast a fraction
    // of the batch and let the rest through unexamined. The cost is that a
    // directory copy queued while an earlier batch is still running pauses that
    // batch between jobs until enumeration completes — accepted, because the
    // queue moves one file at a time regardless.
    bool                       enumerating_   = false;
    // Set when a batch is cancelled with a listing still at the server. Its
    // answer is discarded rather than absorbed into a queue the user emptied.
    bool                       enumCancelled_ = false;
    std::deque<PendingDir>     toVisit_;
    transport::FsError         enumError_;
    std::vector<std::function<void(transport::FsError)>> enumCallbacks_;

    // Destination directories the copy must make, in the order they were
    // discovered — breadth-first, so a parent always precedes its children.
    //
    // Held rather than created during enumeration because enumeration happens
    // before the user has agreed to anything: a declined copy must leave the
    // destination exactly as it found it.
    std::deque<PendingDirCreate> dirsToCreate_;

    bool                       paused_    = false;
    Preflight                  preflight_ = Preflight::Pending;
    // The highest job id the standing verdict actually accounted for.
    //
    // A cleared batch is cleared only for the jobs it examined. Work queued
    // while that batch is still running is new work, and job ids only ever
    // increase, so anything above this watermark has never been weighed against
    // any volume — without it a second copy started before the first finished
    // would inherit "this fits" from a batch it has nothing to do with.
    JobId                         checkedThrough_ = kInvalidJobId;
    std::optional<PreflightReport> lastPreflight_;

    // Directories skipped while assembling the current batch. Reset when a new
    // batch starts — at the first walk of a fresh group, and again whenever the
    // queue drains, since a batch may begin either way.
    size_t                        unreadableDirs_ = 0;
    transport::FsError            firstUnreadable_;
    std::string                   firstUnreadablePath_;

    std::vector<TransferJob> jobs_;
    JobId                    nextId_   = 1;
    JobId                    activeId_ = kInvalidJobId;
    ConflictPolicy           policy_   = ConflictPolicy::Ask;
    SymlinkPolicy            symlinks_ = SymlinkPolicy::Preserve;

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
