#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/TransferQueue.h"

#include <filesystem>
#include <fstream>
#include <memory>

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

JobState StateOf(const TransferQueue& q, JobId id)
{
    const TransferJob* job = q.FindJob(id);
    REQUIRE(job != nullptr);
    return job->state;
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetListener(&listener);

    const JobId id = q.EnqueueUpload(src, "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    const JobId id = q.EnqueueUpload(src, "/remote/a.txt", 5);
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
    TransferQueue q(fs, exec.AsDispatcher());

    const JobId a = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    const JobId b = q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Skip);

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Overwrite);

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Rename);

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Rename);

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    REQUIRE(q.Policy() == ConflictPolicy::Ask);

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    exec.RunAll();

    REQUIRE(StateOf(q, id) == JobState::Skipped);
    REQUIRE(fs.transfers.empty());
}

TEST_CASE("given the ask policy when the prompt answers then the job follows the answer") {
    TempDir tmp("ask_answer");
    FakeRemoteFileSystem fs;
    fs.AddFile("/remote/a.txt", "a.txt", 10);

    ManualExecutor exec;
    TransferQueue q(fs, exec.AsDispatcher());

    int prompts = 0;
    q.SetConflictPrompt([&](const TransferJob&, auto respond) {
        ++prompts;
        respond(ConflictResolution::Overwrite, false);
    });

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    int prompts = 0;
    q.SetConflictPrompt([&](const TransferJob&, auto respond) {
        ++prompts;
        respond(ConflictResolution::Skip, true);
    });

    const JobId a = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    const JobId b = q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetConflictPolicy(ConflictPolicy::Rename);

    const JobId id = q.EnqueueDownload("/remote/a.txt", tmp.Sub("a.txt"));
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
    TransferQueue q(fs, exec.AsDispatcher());

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    const JobId a = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    const JobId b = q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    const JobId a = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    const JobId b = q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());
    q.SetListener(&listener);

    const JobId id = q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    exec.RunAll();
    REQUIRE(StateOf(q, id) == JobState::Active);

    q.CancelJob(id);
    exec.RunAll();

    REQUIRE(fs.cancelCalls.size() == 1);
    REQUIRE(StateOf(q, id) == JobState::Cancelled);
    REQUIRE(q.IsIdle());
}

TEST_CASE("given a mixed queue when everything is cancelled then no job is left pending") {
    TempDir tmp("cancel_all");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(fs, exec.AsDispatcher());

    q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt");
    q.EnqueueUpload(tmp.File("c.txt"), "/remote/c.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
    const JobId b = q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt");
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
        TransferQueue q(fs, exec.AsDispatcher());
        q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt", 100);
    q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt", 200);
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
    TransferQueue q(fs, exec.AsDispatcher());

    q.EnqueueUpload(tmp.File("a.txt"), "/remote/a.txt", 100);
    q.EnqueueUpload(tmp.File("b.txt"), "/remote/b.txt", 100);
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
    TransferQueue q(fs, exec.AsDispatcher());

    bool expanded = false;
    FsError result;
    q.EnqueueDownloadTree("/remote/top", tmp.Sub("dest"),
                          [&](FsError err) { expanded = true; result = std::move(err); });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(result.Ok());
    REQUIRE(q.Jobs().size() == 2);
    REQUIRE(q.TotalBytes() == 30);
    REQUIRE(std::filesystem::is_directory(tmp.Sub("dest")));
    REQUIRE(std::filesystem::is_directory(tmp.Sub("dest/sub")));
}

TEST_CASE("given a symlink in a remote tree when expanded then it is not followed") {
    // Following links is how a recursive copy turns into an infinite one.
    FakeRemoteFileSystem fs;
    TempDir tmp("download_tree_symlink");
    fs.AddDirectory("/remote/top", "top");
    fs.AddFile("/remote/top/real.txt", "real.txt", 10, "/remote/top");
    fs.AddSymlink("/remote/top/loop", "loop", "/remote/top");

    ManualExecutor exec;
    TransferQueue q(fs, exec.AsDispatcher());

    q.EnqueueDownloadTree("/remote/top", tmp.Sub("dest"), nullptr);
    exec.RunAll();

    REQUIRE(q.Jobs().size() == 1);
    REQUIRE(q.Jobs().front().sourcePath == "/remote/top/real.txt");
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
    TransferQueue q(fs, exec.AsDispatcher());

    bool expanded = false;
    FsError result;
    q.EnqueueDownloadTree("/remote/top", tmp.Sub("dest"),
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
    TransferQueue q(fs, exec.AsDispatcher());

    bool expanded = false;
    q.EnqueueUploadTree(tmp.Sub("top"), "/remote/top",
                        [&](FsError) { expanded = true; });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(q.Jobs().size() == 2);

    // The remote subdirectory must be created before its file is queued,
    // otherwise the upload lands in a directory that does not exist yet.
    REQUIRE(fs.mkdirCalls.size() == 1);
    REQUIRE(fs.mkdirCalls.front() == "/remote/top/sub");
}

TEST_CASE("given a missing local directory when expanded for upload then the error is reported") {
    TempDir tmp("upload_tree_missing");
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    TransferQueue q(fs, exec.AsDispatcher());

    FsError result;
    bool expanded = false;
    q.EnqueueUploadTree(tmp.Sub("nope"), "/remote/top",
                        [&](FsError err) { expanded = true; result = std::move(err); });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(result.code == FsErrorCode::LocalIoError);
    REQUIRE(q.Jobs().empty());
}
