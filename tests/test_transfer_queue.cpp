#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/TransferQueue.h"
#include "transport/LocalFileSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

using namespace term::fs;
using term::transport::FsError;
using term::transport::FsErrorCode;
using testing::FakeRemoteFileSystem;
using testing::ManualExecutor;

namespace {

struct RecordingListener : ITransferQueueListener {
    std::vector<JobId> added;
    std::vector<JobId> changed;
    int idleCount = 0;

    void OnTransferJobAdded(JobId id) override   { added.push_back(id); }
    void OnTransferJobChanged(JobId id) override { changed.push_back(id); }
    void OnTransferQueueIdle() override          { ++idleCount; }
};

// A real temp directory, since the local half of a transfer genuinely touches
// the filesystem and faking std::filesystem would test nothing.
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const std::string& tag)
        : path(std::filesystem::temp_directory_path() / ("nate_xferq_" + tag))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }

    std::string File(const std::string& name, const std::string& content = "x") const
    {
        const auto p = path / name;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p);
        out << content;
        return p.string();
    }
    std::string Sub(const std::string& name) const { return (path / name).string(); }
};

// The two endpoints every test builds from. The local one is the real adapter
// over std::filesystem, so the local half of a transfer is exercised rather
// than faked.
term::transport::LocalFileSystem& LocalFs()
{
    static term::transport::LocalFileSystem instance;
    return instance;
}

TransferEndpoint Local()  { return {&LocalFs(), "This computer"}; }
TransferEndpoint Remote(FakeRemoteFileSystem& fs) { return {&fs, "remote"}; }

JobState StateOf(const TransferQueue& q, JobId id)
{
    const TransferJob* job = q.FindJob(id);
    REQUIRE(job != nullptr);
    return job->state;
}

std::string ContentsOf(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// Drains the executor until done() holds, leaving room for the local adapter's
// worker thread to make progress. Every other test here drives a fake that
// answers on the calling thread; a copy between two local paths is the one case
// where the queue is genuinely waiting on another thread.
//
// Returns false on timeout, so a job that never retires fails the test instead
// of hanging the run.
bool RunUntil(ManualExecutor& exec, const std::function<bool()>& done)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        exec.RunAll();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Candidate naming
// ---------------------------------------------------------------------------

TEST_CASE("given a colliding name when a candidate is built then the suffix precedes the extension") {
    REQUIRE(MakeUniqueCandidate("notes.txt", 1) == "notes (1).txt");
    REQUIRE(MakeUniqueCandidate("notes.txt", 2) == "notes (2).txt");
    REQUIRE(MakeUniqueCandidate("archive.tar.gz", 1) == "archive.tar (1).gz");
}

TEST_CASE("given a name without an extension when a candidate is built then the suffix is appended") {
    REQUIRE(MakeUniqueCandidate("README", 1) == "README (1)");
}

TEST_CASE("given a dotfile when a candidate is built then the leading dot is not an extension") {
    REQUIRE(MakeUniqueCandidate(".bashrc", 1) == ".bashrc (1)");
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_CASE("given a queued upload with no collision when run then it completes") {
    TempDir tmp("upload_ok");
    const std::string src = tmp.File("a.txt", "hello");

    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    RecordingListener listener;
    TransferQueue q(exec.AsDispatcher());
    q.SetListener(&listener);

    const JobId id = q.Enqueue(Local(), src, Remote(fs), "/remote/a.txt");
    REQUIRE(listener.added == std::vector<JobId>{id});

    exec.RunAll();                    // conflict check resolves: nothing there
    REQUIRE(StateOf(q, id) == JobState::Active);
    REQUIRE(fs.ActiveCount() == 1);

    fs.ProgressActive(3, 5);
    exec.RunAll();
    REQUIRE(q.FindJob(id)->transferredBytes == 3);
    REQUIRE(q.FindJob(id)->totalBytes == 5);

    fs.CompleteActive();
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Completed);
    REQUIRE(q.IsIdle());
    REQUIRE(listener.idleCount >= 1);
}

TEST_CASE("given a job that completes when progress never reported then transferred matches total") {
    // A progress bar must not be left stuck at 99% by a transport that only
    // reports completion.
    TempDir tmp("upload_no_progress");
    const std::string src = tmp.File("a.txt", "hello");

    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId id = q.Enqueue(Local(), src, Remote(fs), "/remote/a.txt", {5});
    exec.RunAll();
    fs.CompleteActive();
    exec.RunAll();

    REQUIRE(q.FindJob(id)->transferredBytes == 5);
    REQUIRE(q.TransferredBytes() == q.TotalBytes());
}

TEST_CASE("given several queued jobs when run then only one is active at a time") {
    TempDir tmp("sequential");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId a = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    const JobId b = q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    exec.RunAll();

    REQUIRE(StateOf(q, a) == JobState::Active);
    REQUIRE(StateOf(q, b) == JobState::Queued);
    REQUIRE(fs.ActiveCount() == 1);

    fs.CompleteActive();
    exec.RunAll();

    REQUIRE(StateOf(q, a) == JobState::Completed);
    REQUIRE(StateOf(q, b) == JobState::Active);

    fs.CompleteActive();
    exec.RunAll();
    REQUIRE(q.IsIdle());
    REQUIRE(q.PendingCount() == 0);
}

// ---------------------------------------------------------------------------
// Conflict policy
// ---------------------------------------------------------------------------

TEST_CASE("given a colliding destination when the policy is skip then no transfer is issued") {
    TempDir tmp("skip");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Skip);

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Skipped);
    REQUIRE(fs.transfers.empty());
    REQUIRE(q.IsIdle());
}

TEST_CASE("given a colliding destination when the policy is overwrite then the transfer proceeds") {
    TempDir tmp("overwrite");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Overwrite);

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Active);
    REQUIRE(fs.transfers.size() == 1);
    REQUIRE(fs.transfers.front().dest == "/remote/a.txt");
}

TEST_CASE("given a colliding destination when the policy is rename then a free name is chosen") {
    TempDir tmp("rename");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Rename);

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(q.FindJob(id)->destPath == "/remote/a (1).txt");
    REQUIRE(fs.transfers.front().dest == "/remote/a (1).txt");
}

TEST_CASE("given several colliding candidates when renaming then probing continues until one is free") {
    TempDir tmp("rename_probe");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);
    fs.AddFile("/remote/a (1).txt", "a (1).txt", 10);
    fs.AddFile("/remote/a (2).txt", "a (2).txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Rename);

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(q.FindJob(id)->destPath == "/remote/a (3).txt");
}

TEST_CASE("given the ask policy with no prompt installed when a collision occurs then the job is skipped") {
    // A queue that cannot ask must never guess "overwrite" — the only
    // data-safe default is to leave the existing file alone.
    TempDir tmp("ask_no_prompt");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    REQUIRE(q.Policy() == ConflictPolicy::Ask);

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Skipped);
    REQUIRE(fs.transfers.empty());
}

TEST_CASE("given the ask policy when the prompt answers then the job follows the answer") {
    TempDir tmp("ask_answer");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    int prompts = 0;
    q.SetConflictPrompt([&](const TransferJob&, auto respond) {
        ++prompts;
        respond(ConflictResolution::Overwrite, false);
    });

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(prompts == 1);
    REQUIRE(StateOf(q, id) == JobState::Active);
}

TEST_CASE("given an answer marked apply to all when a later job collides then it is not asked again") {
    TempDir tmp("ask_all");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);
    fs.AddFile("/remote/b.txt", "b.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    int prompts = 0;
    q.SetConflictPrompt([&](const TransferJob&, auto respond) {
        ++prompts;
        respond(ConflictResolution::Skip, true);
    });

    const JobId a = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    const JobId b = q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    exec.RunAll();

    REQUIRE(prompts == 1);
    REQUIRE(q.Policy() == ConflictPolicy::Skip);
    REQUIRE(StateOf(q, a) == JobState::Skipped);
    REQUIRE(StateOf(q, b) == JobState::Skipped);
}

TEST_CASE("given a download whose local destination exists when the policy is rename then the local name is freed") {
    TempDir tmp("download_rename");
    tmp.File("a.txt", "existing");

    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Rename);

    const JobId id = q.Enqueue(Remote(fs), "/remote/a.txt", Local(), tmp.Sub("a.txt"));
    exec.RunAll();

    REQUIRE(q.FindJob(id)->destPath == tmp.Sub("a (1).txt"));
    REQUIRE(StateOf(q, id) == JobState::Active);
}

// ---------------------------------------------------------------------------
// Failure and cancellation
// ---------------------------------------------------------------------------

TEST_CASE("given a transfer that fails when it reports then the error is preserved") {
    TempDir tmp("fail");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();
    fs.CompleteActive(FsError::Make(FsErrorCode::PermissionDenied, "denied"));
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Failed);
    REQUIRE(q.FindJob(id)->error.code == FsErrorCode::PermissionDenied);
}

TEST_CASE("given a failing job when it retires then the queue moves on to the next") {
    // One bad file must not strand the rest of the queue behind it.
    TempDir tmp("fail_continue");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId a = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    const JobId b = q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    exec.RunAll();
    fs.CompleteActive(FsError::Make(FsErrorCode::PermissionDenied, "denied"));
    exec.RunAll();

    REQUIRE(StateOf(q, a) == JobState::Failed);
    REQUIRE(StateOf(q, b) == JobState::Active);
}

TEST_CASE("given a queued job when cancelled then it never reaches the transport") {
    TempDir tmp("cancel_queued");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId a = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    const JobId b = q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    exec.RunAll();
    REQUIRE(StateOf(q, b) == JobState::Queued);

    q.CancelJob(b);
    exec.RunAll();

    REQUIRE(StateOf(q, b) == JobState::Cancelled);
    REQUIRE(fs.transfers.size() == 1);            // only job a was ever started
    REQUIRE(fs.transfers.front().dest == "/remote/a.txt");
    REQUIRE(StateOf(q, a) == JobState::Active);
}

TEST_CASE("given an active job when cancelled then the transport is asked and the job retires once") {
    TempDir tmp("cancel_active");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    RecordingListener listener;
    TransferQueue q(exec.AsDispatcher());
    q.SetListener(&listener);

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();
    REQUIRE(StateOf(q, id) == JobState::Active);

    q.CancelJob(id);
    exec.RunAll();

    REQUIRE(fs.cancelCalls.size() == 1);
    REQUIRE(StateOf(q, id) == JobState::Cancelled);
    REQUIRE(q.IsIdle());
}

TEST_CASE("given an active transfer when cancelled then the queue reports cancelling until the transport confirms") {
    TempDir tmp("cancelling_active");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();
    REQUIRE(StateOf(q, id) == JobState::Active);
    REQUIRE_FALSE(q.IsCancelling());

    q.CancelJob(id);
    // The transport has been asked and has not answered. The queue is neither
    // running nor stopped, which is the whole reason this state has a name.
    REQUIRE(q.IsCancelling());

    exec.RunAll();
    REQUIRE(StateOf(q, id) == JobState::Cancelled);
    REQUIRE_FALSE(q.IsCancelling());
}

TEST_CASE("given only queued jobs when cancelled then the queue never reports cancelling") {
    // Nothing is in flight, so the stop is instantaneous. A window that lit up
    // for a state lasting no time at all would flicker for every cancel.
    TempDir tmp("cancelling_queued");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId a = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    const JobId b = q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    exec.RunAll();
    REQUIRE(StateOf(q, a) == JobState::Active);

    q.CancelJob(b);
    REQUIRE_FALSE(q.IsCancelling());
    REQUIRE(StateOf(q, b) == JobState::Cancelled);
}

TEST_CASE("given a transfer that finished before the cancel arrived then the queue stops reporting cancelling") {
    // The race the flag must not outlive: the file lands, and the user clicks
    // Cancel before the completion has been delivered. The job retires as
    // Completed, and a queue still claiming to be stopping would say so for the
    // rest of its life.
    TempDir tmp("cancelling_race");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId id = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    exec.RunAll();
    REQUIRE(StateOf(q, id) == JobState::Active);

    fs.CompleteActive();          // done at the server; the queue has not heard
    q.CancelAll();
    REQUIRE(q.IsCancelling());

    exec.RunAll();
    REQUIRE(StateOf(q, id) == JobState::Completed);
    REQUIRE_FALSE(q.IsCancelling());
}

TEST_CASE("given a tree still being walked when cancelled then the queue reports cancelling until the listing lands") {
    TempDir tmp("cancelling_walk");
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/remote/top", "top");
    fs.AddFile("/remote/top/a.txt", "a.txt", 10, "/remote/top");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.EnqueueTree(Remote(fs), "/remote/top", Local(), tmp.Sub("dest"), nullptr);
    REQUIRE(q.IsExpanding());
    REQUIRE_FALSE(q.IsCancelling());

    q.CancelAll();
    // A listing already at the server cannot be unasked, so the walk is still
    // out even though its answer is going to be discarded.
    REQUIRE(q.IsCancelling());

    exec.RunAll();
    REQUIRE_FALSE(q.IsExpanding());
    REQUIRE_FALSE(q.IsCancelling());
}

TEST_CASE("given a mixed queue when everything is cancelled then no job is left pending") {
    TempDir tmp("cancel_all");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    q.Enqueue(Local(), tmp.File("c.txt"), Remote(fs), "/remote/c.txt");
    exec.RunAll();

    q.CancelAll();
    exec.RunAll();

    REQUIRE(q.PendingCount() == 0);
    REQUIRE(q.IsIdle());
}

TEST_CASE("given terminal jobs when cleared then only unfinished work remains") {
    TempDir tmp("clear");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    const JobId b = q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt");
    exec.RunAll();
    fs.CompleteActive();
    exec.RunAll();

    REQUIRE(q.ClearFinished() == 1);
    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs().front().id == b);
}

TEST_CASE("given a transfer in flight when the queue is destroyed then the late callback is inert") {
    // The transport outlives the queue during a session teardown, and still
    // holds the completion callback. It must find nothing to write into.
    TempDir tmp("outlive");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;

    {
        TransferQueue q(exec.AsDispatcher());
        q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
        exec.RunAll();
        REQUIRE(fs.ActiveCount() == 1);
    }   // queue destroyed with a transfer still pending

    fs.CompleteActive();
    REQUIRE_NOTHROW(exec.RunAll());
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------

TEST_CASE("given several jobs when aggregated then totals cover the whole queue") {
    TempDir tmp("aggregate");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt", {100});
    q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt", {200});
    exec.RunAll();

    REQUIRE(q.TotalBytes() == 300);
    REQUIRE(q.TransferredBytes() == 0);

    fs.ProgressActive(40, 100);
    exec.RunAll();
    REQUIRE(q.TransferredBytes() == 40);
}

TEST_CASE("given a completed job when more work follows then aggregate progress never regresses") {
    TempDir tmp("aggregate_monotonic");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt", {100});
    q.Enqueue(Local(), tmp.File("b.txt"), Remote(fs), "/remote/b.txt", {100});
    exec.RunAll();
    fs.CompleteActive();
    exec.RunAll();

    const uint64_t afterFirst = q.TransferredBytes();
    REQUIRE(afterFirst == 100);

    fs.ProgressActive(10, 100);
    exec.RunAll();
    REQUIRE(q.TransferredBytes() >= afterFirst);
}

// ---------------------------------------------------------------------------
// Recursive expansion
// ---------------------------------------------------------------------------

TEST_CASE("given a remote tree when expanded then every file becomes a job and directories are created") {
    TempDir tmp("download_tree");
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/remote/top", "top");
    fs.AddFile("/remote/top/a.txt", "a.txt", 10, "/remote/top");
    fs.AddDirectory("/remote/top/sub", "sub", "/remote/top");
    fs.AddFile("/remote/top/sub/b.txt", "b.txt", 20, "/remote/top/sub");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    bool expanded = false;
    FsError result;
    q.EnqueueTree(Remote(fs), "/remote/top", Local(), tmp.Sub("dest"),
                  [&](FsError err) { expanded = true; result = std::move(err); });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(result.Ok());
    REQUIRE(q.Jobs().size() == 2);
    REQUIRE(q.TotalBytes() == 30);
    REQUIRE(std::filesystem::is_directory(tmp.Sub("dest")));
    REQUIRE(std::filesystem::is_directory(tmp.Sub("dest/sub")));
}

TEST_CASE("given a symlink in a remote tree when expanded then it is never descended into") {
    // Descending is how a recursive copy turns into an infinite one, and no
    // policy changes that: a link is dealt with where it stands. What differs
    // between policies is whether it is reproduced or left out, not whether
    // the walk goes through it.
    FakeRemoteFileSystem fs;
    TempDir tmp("download_tree_symlink");
    fs.AddDirectory("/remote/top", "top");
    fs.AddFile("/remote/top/real.txt", "real.txt", 10, "/remote/top");
    fs.AddSymlink("/remote/top/loop", "loop", "/remote/top");

    SECTION("preserved") {
        ManualExecutor exec;
        TransferQueue q(exec.AsDispatcher());
        q.SetSymlinkPolicy(SymlinkPolicy::Preserve);

        q.EnqueueTree(Remote(fs), "/remote/top", Local(), tmp.Sub("dest"), nullptr);
        exec.RunAll();

        REQUIRE(q.Jobs().size() == 2);
        REQUIRE(q.Jobs()[0].sourcePath == "/remote/top/real.txt");
        REQUIRE(q.Jobs()[1].sourcePath == "/remote/top/loop");
        REQUIRE(q.Jobs()[1].kind == JobKind::Link);
        // The link was listed as an entry, never opened as a directory.
        REQUIRE(fs.listCalls == std::vector<std::string>{"/remote/top"});
    }

    SECTION("skipped") {
        ManualExecutor exec;
        TransferQueue q(exec.AsDispatcher());
        q.SetSymlinkPolicy(SymlinkPolicy::Skip);

        q.EnqueueTree(Remote(fs), "/remote/top", Local(), tmp.Sub("dest"), nullptr);
        exec.RunAll();

        REQUIRE(q.Jobs().size() == 2);
        REQUIRE(q.Jobs()[1].state == JobState::Skipped);
        REQUIRE(fs.listCalls == std::vector<std::string>{"/remote/top"});
    }
}

TEST_CASE("given a partially readable remote tree when expanded then readable files are still queued") {
    TempDir tmp("download_tree_partial");
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/remote/top", "top");
    fs.AddFile("/remote/top/a.txt", "a.txt", 10, "/remote/top");
    fs.AddDirectory("/remote/top/secret", "secret", "/remote/top");
    fs.listErrors["/remote/top/secret"] =
        FsError::Make(FsErrorCode::PermissionDenied, "denied");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    bool expanded = false;
    FsError result;
    q.EnqueueTree(Remote(fs), "/remote/top", Local(), tmp.Sub("dest"),
                  [&](FsError err) { expanded = true; result = std::move(err); });
    exec.RunAll();

    REQUIRE(expanded);
    // The unreadable subdirectory is reported, but the file that *was*
    // readable is queued rather than discarded.
    REQUIRE(result.Failed());
    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs().front().sourcePath == "/remote/top/a.txt");
}

TEST_CASE("given a local tree when expanded for upload then directories precede their files") {
    TempDir tmp("upload_tree");
    tmp.File("top/a.txt", "aa");
    tmp.File("top/sub/b.txt", "bbb");

    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    bool expanded = false;
    q.EnqueueTree(Local(), tmp.Sub("top"), Remote(fs), "/remote/top",
                  [&](FsError) { expanded = true; });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(q.Jobs().size() == 2);

    // The destination root must be created before anything lands in it, and
    // each subdirectory before its own contents — otherwise every file in an
    // uploaded directory fails against a path that does not exist yet.
    REQUIRE(fs.mkdirCalls.size() == 2);
    REQUIRE(fs.mkdirCalls[0] == "/remote/top");
    REQUIRE(fs.mkdirCalls[1] == "/remote/top/sub");
}

TEST_CASE("given a missing local directory when expanded for upload then the error is reported") {
    TempDir tmp("upload_tree_missing");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    FsError result;
    bool expanded = false;
    q.EnqueueTree(Local(), tmp.Sub("nope"), Remote(fs), "/remote/top",
                  [&](FsError err) { expanded = true; result = std::move(err); });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(result.Failed());
    REQUIRE(q.Jobs().empty());
}

// ---------------------------------------------------------------------------
// Server to server
// ---------------------------------------------------------------------------

TEST_CASE("given two remote endpoints when a file is queued then it is staged through this machine") {
    // SFTP has no server-to-server copy, so the bytes must come down and go
    // back up. The queue does that rather than refusing the transfer.
    FakeRemoteFileSystem src, dst;
    src.AddFile("/src/report.txt", "report.txt", 100);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId id = q.Enqueue(Remote(src), "/src/report.txt",
                               Remote(dst), "/dst/report.txt", {100});
    exec.RunAll();

    REQUIRE(q.FindJob(id)->viaLocalStaging);
    // The bytes genuinely travel twice, and the denominator says so.
    REQUIRE(q.FindJob(id)->totalBytes == 200);

    // First leg: a download from the source into a staging file.
    REQUIRE(src.ActiveCount() == 1);
    REQUIRE(dst.ActiveCount() == 0);
    const std::string staged = src.transfers.front().dest;
    REQUIRE_FALSE(staged.empty());
    REQUIRE(staged != "/dst/report.txt");

    src.CompleteActive();
    exec.RunAll();

    // Second leg: an upload to the destination from that same staging file.
    REQUIRE(dst.ActiveCount() == 1);
    REQUIRE(dst.transfers.front().source == staged);
    REQUIRE(dst.transfers.front().dest == "/dst/report.txt");

    dst.CompleteActive();
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Completed);
    REQUIRE(q.IsIdle());
}

TEST_CASE("given a staged transfer when the first leg fails then the second never runs") {
    FakeRemoteFileSystem src, dst;
    src.AddFile("/src/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId id = q.Enqueue(Remote(src), "/src/a.txt", Remote(dst), "/dst/a.txt", {10});
    exec.RunAll();

    src.CompleteActive(FsError::Make(FsErrorCode::PermissionDenied, "denied"));
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Failed);
    REQUIRE(dst.transfers.empty());
}

TEST_CASE("given a staged transfer when it completes then the staging file is removed") {
    // The scratch copy is an implementation detail of the route; leaving it
    // behind would quietly fill the user's temp directory.
    FakeRemoteFileSystem src, dst;
    src.AddFile("/src/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.Enqueue(Remote(src), "/src/a.txt", Remote(dst), "/dst/a.txt", {10});
    exec.RunAll();

    const std::string staged = src.transfers.front().dest;
    // The fake never writes it, so create it to prove the queue removes it.
    { std::ofstream out(staged); out << "body"; }
    REQUIRE(std::filesystem::exists(staged));

    src.CompleteActive();
    exec.RunAll();
    dst.CompleteActive();
    exec.RunAll();

    REQUIRE_FALSE(std::filesystem::exists(staged));
}

TEST_CASE("given a staged transfer when the conflict check runs then it asks the destination") {
    // The check must reach the far server, not the staging path.
    FakeRemoteFileSystem src, dst;
    src.AddFile("/src/a.txt", "a.txt", 10);
    dst.AddFile("/dst/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Skip);

    const JobId id = q.Enqueue(Remote(src), "/src/a.txt", Remote(dst), "/dst/a.txt", {10});
    exec.RunAll();

    REQUIRE(dst.statCalls == std::vector<std::string>{"/dst/a.txt"});
    REQUIRE(StateOf(q, id) == JobState::Skipped);
    REQUIRE(src.transfers.empty());
}

TEST_CASE("given a remote to remote tree when expanded then both servers are walked correctly") {
    FakeRemoteFileSystem src, dst;
    src.AddDirectory("/src/top", "top");
    src.AddFile("/src/top/a.txt", "a.txt", 10, "/src/top");
    src.AddDirectory("/src/top/sub", "sub", "/src/top");
    src.AddFile("/src/top/sub/b.txt", "b.txt", 20, "/src/top/sub");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    bool expanded = false;
    q.EnqueueTree(Remote(src), "/src/top", Remote(dst), "/dst/top",
                  [&](FsError) { expanded = true; });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(q.Jobs().size() == 2);
    // Directories are created on the destination, listings read from the source.
    REQUIRE(dst.mkdirCalls == std::vector<std::string>{"/dst/top", "/dst/top/sub"});
    REQUIRE(src.listCalls.size() == 2);
}

// ---------------------------------------------------------------------------
// Endpoint loss
// ---------------------------------------------------------------------------

TEST_CASE("given a session that closes when its jobs are cancelled then unrelated work survives") {
    FakeRemoteFileSystem doomed, healthy;
    TempDir tmp("endpoint_loss");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId onDoomed  = q.Enqueue(Local(), tmp.File("a.txt"),
                                      Remote(doomed), "/remote/a.txt");
    const JobId onHealthy = q.Enqueue(Local(), tmp.File("b.txt"),
                                      Remote(healthy), "/remote/b.txt");
    exec.RunAll();

    q.CancelJobsUsing(&doomed);
    exec.RunAll();

    REQUIRE(StateOf(q, onDoomed) == JobState::Cancelled);
    REQUIRE(q.FindJob(onDoomed)->error.code == FsErrorCode::NotConnected);
    // The other session had nothing to do with it and must keep running.
    REQUIRE_FALSE(q.FindJob(onHealthy)->IsTerminal());
}

TEST_CASE("given a job whose endpoint vanished before it started then it fails rather than hanging") {
    FakeRemoteFileSystem fs;
    TempDir tmp("endpoint_null");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const JobId blocked = q.Enqueue(Local(), tmp.File("a.txt"), Remote(fs), "/remote/a.txt");
    // A second job with no destination at all, standing in for an endpoint
    // that was torn down between queueing and running.
    const JobId orphan  = q.Enqueue(Local(), tmp.File("b.txt"),
                                    TransferEndpoint{}, "/remote/b.txt");
    exec.RunAll();

    fs.CompleteActive();
    exec.RunAll();

    REQUIRE(StateOf(q, blocked) == JobState::Completed);
    REQUIRE(StateOf(q, orphan) == JobState::Failed);
    REQUIRE(q.FindJob(orphan)->error.code == FsErrorCode::NotConnected);
    REQUIRE(q.IsIdle());
}

// ---------------------------------------------------------------------------
// Adapters that answer inline
// ---------------------------------------------------------------------------
// IRemoteFileSystem lets a callback fire before the call that started it has
// returned — the local-disk adapter does exactly that. Paired with a dispatcher
// that runs work immediately, a whole job can retire inside StartTransfer, and
// the handle the adapter eventually returns then describes a job that is over.

TEST_CASE("given an adapter that completes inline when a later job is active "
          "then cancelling reaches the right transfer") {
    TempDir tmp("inline_handle");

    // Runs posted work at once, which is what a dispatcher does when the caller
    // already sits on the owning thread.
    TransferQueue q([](std::function<void()> fn) { fn(); });

    FakeRemoteFileSystem pending;                       // transfers stay in flight
    FakeRemoteFileSystem inlineFs;
    inlineFs.completeTransfersInline = true;

    // Downloads: the destination is the local disk, so the source moves bytes.
    const JobId first  = q.Enqueue(Remote(pending), "/remote/a.bin",
                                   Local(), tmp.Sub("a.bin"));
    // Queued behind `first`, and finishes the instant it starts.
    q.Enqueue(Remote(inlineFs), "/remote/b.bin", Local(), tmp.Sub("b.bin"));
    // Queued behind that, and is the job left running at the end.
    const JobId last   = q.Enqueue(Remote(pending), "/remote/c.bin",
                                   Local(), tmp.Sub("c.bin"));

    REQUIRE(StateOf(q, first) == JobState::Active);

    // Retiring the first lets the inline job run and finish within the same
    // call that then starts `last`.
    pending.CompleteActive();

    REQUIRE(StateOf(q, first) == JobState::Completed);
    REQUIRE(StateOf(q, last)  == JobState::Active);

    // The handle recorded for the active job must be its own. If the inline
    // job's handle overwrote it on the way out, this cancel goes to a transfer
    // that no longer exists and `last` never retires.
    q.CancelJob(last);
    REQUIRE(StateOf(q, last) == JobState::Cancelled);
    REQUIRE(q.IsIdle());
}

// ---------------------------------------------------------------------------
// Item routing
//
// What a picked item means — one transfer or a whole walk — is policy, and it
// lives here rather than in whichever view did the picking.
// ---------------------------------------------------------------------------

TEST_CASE("given a picked file when queued as an item then it becomes one transfer") {
    TempDir tmp("item_file");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const TransferItem item{tmp.File("a.txt", "hello"), "a.txt", false, false, 5};
    bool expanded = false;
    q.EnqueueItem(Local(), item, Remote(fs), "/remote",
                  [&](FsError err) { expanded = err.Ok(); });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs()[0].destPath == "/remote/a.txt");
    REQUIRE(fs.mkdirCalls.empty());     // a file needs no directory made for it
}

TEST_CASE("given a picked directory when queued as an item then it is walked") {
    TempDir tmp("item_dir");
    tmp.File("tree/one.txt", "1");
    tmp.File("tree/two.txt", "22");

    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    const TransferItem item{tmp.Sub("tree"), "tree", true, false, 0};
    bool expanded = false;
    q.EnqueueItem(Local(), item, Remote(fs), "/remote",
                  [&](FsError err) { expanded = err.Ok(); });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(q.Jobs().size() == 2);
    REQUIRE(fs.mkdirCalls == std::vector<std::string>{"/remote/tree"});
}


// ---------------------------------------------------------------------------
// Symlinks
//
// There is no right answer here, which is why it is a policy and not a rule.
// What these pin down is that the choice is honoured and that a link is never
// quietly dropped or silently turned into something else.
// ---------------------------------------------------------------------------

TEST_CASE("given a local path when described for transfer then a link is reported as a link") {
    // The question the two construction sites used to answer differently.
    // std::filesystem::is_directory follows the link and would say "directory";
    // a listing says "symlink", and this has to agree with the listing.
    TempDir tmp("describe_local");
    tmp.File("real/inside.txt", "x");
    std::error_code ec;
    std::filesystem::create_directory_symlink(tmp.Sub("real"), tmp.Sub("link"), ec);
    if (ec) return;   // no symlink support here; nothing to assert

    const TransferItem link = ItemForLocalPath(tmp.Sub("link"));
    REQUIRE(link.name == "link");
    REQUIRE(link.isSymlink);
    REQUIRE_FALSE(link.isDir);        // the link, not what it points at

    const TransferItem dir = ItemForLocalPath(tmp.Sub("real"));
    REQUIRE(dir.isDir);
    REQUIRE_FALSE(dir.isSymlink);

    const TransferItem file = ItemForLocalPath(tmp.File("plain.txt", "hello"));
    REQUIRE_FALSE(file.isDir);
    REQUIRE_FALSE(file.isSymlink);
    REQUIRE(file.size == 5);
}

TEST_CASE("given the preserve policy when a link is copied then it is reproduced not followed") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    src.AddFile("/src/latest", "latest", 0);
    src.existing["/src/latest"].isSymlink = true;

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetSymlinkPolicy(SymlinkPolicy::Preserve);

    TransferItem item;
    item.path = "/src/latest";
    item.name = "latest";
    item.isSymlink = true;

    q.EnqueueItem(Remote(src), item, Remote(dst), "/dst", {});
    exec.RunAll();

    // Read from the source, written verbatim at the destination. No transfer
    // was started for it, because there are no bytes to move.
    REQUIRE(dst.symlinkCalls.size() == 1);
    REQUIRE(dst.symlinkCalls[0].linkPath == "/dst/latest");
    REQUIRE(dst.symlinkCalls[0].target == "/src/latest-target");
    REQUIRE(src.transfers.empty());
    REQUIRE(dst.transfers.empty());
    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs()[0].state == JobState::Completed);
}

TEST_CASE("given the skip policy when a link is copied then it is recorded rather than dropped") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetSymlinkPolicy(SymlinkPolicy::Skip);

    TransferItem item;
    item.path = "/src/latest";
    item.name = "latest";
    item.isSymlink = true;

    q.EnqueueItem(Remote(src), item, Remote(dst), "/dst", {});
    exec.RunAll();

    // Visible in the queue, so a copy never silently omits something.
    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs()[0].state == JobState::Skipped);
    REQUIRE(dst.symlinkCalls.empty());
    REQUIRE(q.IsIdle());
}

TEST_CASE("given a tree containing links when expanded then each link follows the policy") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    src.AddDirectory("/src/tree", "tree");
    src.AddFile("/src/tree/real.txt", "real.txt", 4, "/src/tree");
    src.AddFile("/src/tree/link", "link", 0, "/src/tree");
    src.listings["/src/tree"][1].isSymlink = true;

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetSymlinkPolicy(SymlinkPolicy::Preserve);

    TransferItem item;
    item.path  = "/src/tree";
    item.name  = "tree";
    item.isDir = true;

    q.EnqueueItem(Remote(src), item, Remote(dst), "/dst", {});
    exec.RunAll();

    // The file is queued first and holds the queue while its bytes move; the
    // link waits its turn like any other job. Server-to-server stages through
    // here, so both legs have to finish.
    src.CompleteActive();
    exec.RunAll();
    dst.CompleteActive();
    exec.RunAll();

    // One byte-moving job for the file, one link reproduced. The walk never
    // descended into the link, which is what keeps it free of cycles.
    REQUIRE(q.Jobs().size() == 2);
    REQUIRE(dst.symlinkCalls.size() == 1);
    REQUIRE(dst.symlinkCalls[0].linkPath == "/dst/tree/link");
    REQUIRE(src.listCalls == std::vector<std::string>{"/src/tree"});
}

TEST_CASE("given a server that cannot create links when one is preserved then the job fails plainly") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.symlinkUnsupported = true;
    src.AddFile("/src/latest", "latest", 0);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetSymlinkPolicy(SymlinkPolicy::Preserve);

    TransferItem item;
    item.path = "/src/latest";
    item.name = "latest";
    item.isSymlink = true;

    q.EnqueueItem(Remote(src), item, Remote(dst), "/dst", {});
    exec.RunAll();

    // Reported, not silently downgraded to copying the target: guessing what
    // the user wanted instead is exactly what the policy exists to avoid.
    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs()[0].state == JobState::Failed);
    REQUIRE(q.Jobs()[0].error.code == FsErrorCode::Unsupported);
}

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------

TEST_CASE("given an executable source when queued then the destination is created with its mode") {
    // The bug this covers: an uploaded script that lands without its execute
    // bit is not the file the user copied.
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    TransferItem item;
    item.path = "/local/deploy.sh";
    item.name = "deploy.sh";
    item.size = 40;
    item.mode = 0100755;

    q.EnqueueItem(Local(), item, Remote(fs), "/remote");
    exec.RunAll();

    REQUIRE(fs.transfers.size() == 1);
    REQUIRE(fs.transfers[0].isUpload);
    REQUIRE(fs.transfers[0].sourceMode == 0755);
}

TEST_CASE("given a source whose mode is unknown when queued then none is imposed") {
    // Nothing read the source's permissions, so the adapter's default stands.
    // Inventing one here would be a guess wearing the clothes of a fact.
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    TransferItem item;
    item.path = "/local/a.txt";
    item.name = "a.txt";

    q.EnqueueItem(Local(), item, Remote(fs), "/remote");
    exec.RunAll();

    REQUIRE(fs.transfers.size() == 1);
    REQUIRE_FALSE(fs.transfers[0].sourceMode.has_value());
}

TEST_CASE("given a setuid source when copied then the special bits are not reproduced") {
    // cp(1) drops them, and for the same reason: recreating a setuid binary
    // somewhere the user merely has write access is an escalation waiting to
    // be found by somebody else.
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    TransferItem item;
    item.path = "/local/tool";
    item.name = "tool";
    item.mode = 0104755;   // setuid + rwxr-xr-x

    q.EnqueueItem(Local(), item, Remote(fs), "/remote");
    exec.RunAll();

    REQUIRE(fs.transfers.size() == 1);
    REQUIRE(fs.transfers[0].sourceMode == 0755);
}

TEST_CASE("given a copied tree when walked then each file keeps its own mode") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    src.AddDirectory("/src/tree", "tree");
    src.AddFile("/src/tree/run.sh", "run.sh", 10, "/src/tree", 0100755);
    src.AddFile("/src/tree/notes.txt", "notes.txt", 20, "/src/tree", 0100640);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    q.EnqueueTree(Remote(src), "/src/tree", Remote(dst), "/dst/tree", nullptr);
    exec.RunAll();

    REQUIRE(q.Jobs().size() == 2);
    // Per file, not per walk: one mode applied to the whole tree would be the
    // easy mistake here.
    const TransferJob* script =
        q.Jobs()[0].sourcePath == "/src/tree/run.sh" ? &q.Jobs()[0] : &q.Jobs()[1];
    const TransferJob* notes =
        script == &q.Jobs()[0] ? &q.Jobs()[1] : &q.Jobs()[0];

    REQUIRE(script->sourceMode == 0755);
    REQUIRE(notes->sourceMode == 0640);
}

TEST_CASE("given a staged server-to-server copy when it runs then both legs carry the source mode") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    TransferItem item;
    item.path = "/src/run.sh";
    item.name = "run.sh";
    item.size = 10;
    item.mode = 0100755;

    q.EnqueueItem(Remote(src), item, Remote(dst), "/dst");
    exec.RunAll();

    // Leg one: down to the staging file, which must not sit in /tmp with
    // permissions the original never had.
    REQUIRE(src.transfers.size() == 1);
    REQUIRE(src.transfers[0].sourceMode == 0755);

    src.CompleteActive();
    exec.RunAll();

    // Leg two carries the *source's* mode, not the staging file's: the staging
    // file is an implementation detail and must not decide what the user gets.
    REQUIRE(dst.transfers.size() == 1);
    REQUIRE(dst.transfers[0].sourceMode == 0755);
}

// ---------------------------------------------------------------------------
// Local to local
// ---------------------------------------------------------------------------

TEST_CASE("given both endpoints on this computer when a file is copied then it arrives") {
    // Both panes can be pointed at this computer, so the queue has to route a
    // copy where neither side is remote — it used to hand the job to an adapter
    // that declined it, and the user saw a failed transfer for a fair request.
    TempDir tmp("local_to_local");
    const std::string src = tmp.File("a.txt", "hello");
    const std::string dstDir = tmp.Sub("out");
    std::filesystem::create_directories(dstDir);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    TransferItem item;
    item.path = src;
    item.name = "a.txt";
    item.size = 5;
    item.mode = 0100644;

    q.EnqueueItem(Local(), item, Local(), dstDir);

    const JobId id = q.Jobs().at(0).id;
    REQUIRE(RunUntil(exec, [&] {
        const TransferJob* job = q.FindJob(id);
        return job && job->IsTerminal();
    }));

    REQUIRE(StateOf(q, id) == JobState::Completed);
    REQUIRE(std::filesystem::exists(tmp.Sub("out/a.txt")));
    REQUIRE(ContentsOf(tmp.Sub("out/a.txt")) == "hello");
}

TEST_CASE("given a local copy onto the file itself when queued then it fails without destroying it") {
    // The destination directory is the source's own, so the conflict check
    // finds the file and Overwrite would truncate what is about to be read.
    TempDir tmp("local_self_copy");
    const std::string src = tmp.File("a.txt", "precious");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Overwrite);

    TransferItem item;
    item.path = src;
    item.name = "a.txt";
    item.size = 8;

    q.EnqueueItem(Local(), item, Local(), tmp.path.string());

    const JobId id = q.Jobs().at(0).id;
    REQUIRE(RunUntil(exec, [&] {
        const TransferJob* job = q.FindJob(id);
        return job && job->IsTerminal();
    }));

    REQUIRE(StateOf(q, id) == JobState::Failed);
    REQUIRE(ContentsOf(src) == "precious");
}

TEST_CASE("given a directory copied into its own subtree when queued then it is refused") {
    // The walk would otherwise chase the copies it is creating: each listing
    // turns up the directory written a moment before, and only the depth cap
    // ends it, after filling the tree with nested duplicates.
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/data", "data");
    fs.AddFile("/data/a.txt", "a.txt", 5, "/data");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    FsError reported;
    q.EnqueueTree(Remote(fs), "/data", Remote(fs), "/data/backup",
                  [&](FsError err) { reported = std::move(err); });
    exec.RunAll();

    REQUIRE(reported.Failed());
    REQUIRE(q.Jobs().empty());
    // Refused before anything was written, so no half-made directory is left
    // for the user to clear up.
    REQUIRE(fs.listCalls.empty());
}

TEST_CASE("given the same path on two filesystems when a tree is copied then it proceeds") {
    // Identical paths on two machines are two different places; refusing this
    // would break the ordinary case of mirroring /etc between hosts.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    src.AddDirectory("/data", "data");
    src.AddFile("/data/a.txt", "a.txt", 5, "/data");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher());

    FsError reported;
    q.EnqueueTree(Remote(src), "/data", Remote(dst), "/data",
                  [&](FsError err) { reported = std::move(err); });
    exec.RunAll();

    REQUIRE(reported.Ok());
    REQUIRE(q.Jobs().size() == 1);
}
