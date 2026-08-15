#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/TransferQueue.h"
#include "transport/LocalFileSystem.h"

#include <filesystem>
#include <fstream>
#include <optional>

using namespace term::fs;
using term::transport::FsSpaceInfo;
using testing::FakeRemoteFileSystem;
using testing::ManualExecutor;

namespace {

term::transport::LocalFileSystem& LocalFs()
{
    static term::transport::LocalFileSystem instance;
    return instance;
}

TransferEndpoint Local() { return {&LocalFs(), "This computer"}; }
TransferEndpoint Remote(FakeRemoteFileSystem& fs, const std::string& label)
{
    return {&fs, label};
}

FsSpaceInfo Volume(uint64_t available, bool readOnly = false)
{
    FsSpaceInfo info;
    info.totalBytes     = available * 2;
    info.availableBytes = available;
    info.readOnly       = readOnly;
    return info;
}

// A staging probe that answers with whatever the test set, so a volume that
// does not exist on this machine can still be described.
StagingSpaceProbe Staging(std::optional<FsSpaceInfo> space)
{
    return [space] { return space; };
}

// Records what the queue asked, and answers however the test says to.
struct RecordingPrompt {
    bool                        asked    = false;
    bool                        answer   = true;
    std::optional<PreflightReport> seen;

    SpaceWarningPrompt Handler()
    {
        return [this](const PreflightReport& report,
                      std::function<void(bool)> respond) {
            asked = true;
            seen  = report;
            respond(answer);
        };
    }
};

// A real local file, since an upload's source half genuinely has to exist.
struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::string& tag, size_t bytes = 8)
        : path(std::filesystem::temp_directory_path() / ("nate_preflight_" + tag))
    {
        std::ofstream out(path, std::ios::binary);
        out << std::string(bytes, 'x');
    }
    ~TempFile() { std::error_code ec; std::filesystem::remove(path, ec); }

    std::string Str() const { return path.string(); }
};

} // namespace

// ---------------------------------------------------------------------------
// The check runs before any byte moves
// ---------------------------------------------------------------------------

TEST_CASE("given a batch that fits when queued then no warning is raised and it runs") {
    TempFile src("fits");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1000);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1000)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    const JobId id = q.Enqueue(Local(), src.Str(), Remote(fs, "host"),
                               "/remote/a.txt", {100});
    exec.RunAll();

    REQUIRE_FALSE(prompt.asked);
    REQUIRE(q.FindJob(id)->state == JobState::Active);
}

TEST_CASE("given a batch larger than the destination when queued then the user is warned first") {
    TempFile src("too_big");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(100);

    ManualExecutor exec;
    RecordingPrompt prompt;
    prompt.answer = true;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1000)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    const JobId id = q.Enqueue(Local(), src.Str(), Remote(fs, "host"),
                               "/remote/a.txt", {5000});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.destination.Short());
    REQUIRE(prompt.seen->forecast.destination.ShortfallBytes() == 4900);
    // Answered yes, so the batch went ahead regardless.
    REQUIRE(q.FindJob(id)->state == JobState::Active);
}

TEST_CASE("given a space warning when the user declines then nothing is transferred") {
    TempFile src("declined");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(100);

    ManualExecutor exec;
    RecordingPrompt prompt;
    prompt.answer = false;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1000)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    const JobId id = q.Enqueue(Local(), src.Str(), Remote(fs, "host"),
                               "/remote/a.txt", {5000});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(q.FindJob(id)->state == JobState::Cancelled);
    // Declining must abandon the batch rather than park it: a queue held behind
    // an answered prompt is indistinguishable from one that has hung.
    REQUIRE(q.IsIdle());
    REQUIRE(fs.transfers.empty());
}

TEST_CASE("given the destination volume when checked then the directory is asked about, not the file") {
    // A host commonly has several volumes. The file does not exist yet, so
    // asking about its path would be asking about nothing; the directory it is
    // going into is what identifies the volume.
    TempFile src("volume");
    FakeRemoteFileSystem fs;
    fs.spaceByPath["/remote/data"] = Volume(1000);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1000)));

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/data/a.txt", {100});
    exec.RunAll();

    REQUIRE(fs.spaceCalls.size() == 1);
    REQUIRE(fs.spaceCalls.front() == "/remote/data");
}

// ---------------------------------------------------------------------------
// Staging is charged the largest file, not the total
// ---------------------------------------------------------------------------

TEST_CASE("given several server-to-server copies when checked then staging is charged only the largest") {
    // The queue moves one file at a time and reclaims each staging file as it
    // goes, so a batch needs room for its largest member, not its sum. Charging
    // the sum would refuse most batches on a /tmp sized to a fraction of RAM.
    FakeRemoteFileSystem from;
    FakeRemoteFileSystem to;
    to.defaultSpace = Volume(1ULL << 40);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1000)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.Enqueue(Remote(from, "a"), "/a/one", Remote(to, "b"), "/b/one", {400});
    q.Enqueue(Remote(from, "a"), "/a/two", Remote(to, "b"), "/b/two", {900});
    exec.RunAll();

    REQUIRE_FALSE(prompt.asked);   // 900 fits in 1000; the 1300 total does not apply
}

TEST_CASE("given a single server-to-server file larger than staging when queued then the user is warned") {
    FakeRemoteFileSystem from;
    FakeRemoteFileSystem to;
    to.defaultSpace = Volume(1ULL << 40);   // the destination has ample room

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1000)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.Enqueue(Remote(from, "a"), "/a/big.iso", Remote(to, "b"), "/b/big.iso", {5000});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.staging.Short());
    REQUIRE(prompt.seen->forecast.staging.requiredBytes == 5000);
    REQUIRE_FALSE(prompt.seen->forecast.destination.Short());
}

TEST_CASE("given a staged copy when the destination is charged then the bytes count once") {
    // A staged job's totalBytes counts both legs, because the bytes genuinely
    // travel twice. What lands on the destination is one copy of the file.
    FakeRemoteFileSystem from;
    FakeRemoteFileSystem to;
    to.defaultSpace = Volume(1500);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.Enqueue(Remote(from, "a"), "/a/f", Remote(to, "b"), "/b/f", {1000});
    exec.RunAll();

    REQUIRE_FALSE(prompt.asked);
    REQUIRE(q.LastPreflight()->forecast.destination.requiredBytes == 1000);
}

TEST_CASE("given an ordinary upload when checked then the staging volume is never consulted") {
    TempFile src("nostaging");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40);

    bool probed = false;
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(),
                    [&probed]() -> std::optional<FsSpaceInfo> {
                        probed = true;
                        return Volume(1000);
                    });

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/a.txt", {100});
    exec.RunAll();

    REQUIRE_FALSE(probed);
}

// ---------------------------------------------------------------------------
// Unmeasurable volumes
// ---------------------------------------------------------------------------

TEST_CASE("given a small copy to a server that cannot report free space then it runs unwarned") {
    // statvfs is an OpenSSH extension, not base SFTP. Prompting on every small
    // copy to a server that lacks it, or reporting it as zero space, would both
    // be wrong.
    TempFile src("unsupported_small");
    FakeRemoteFileSystem fs;
    fs.spaceUnsupported = true;

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    const JobId id = q.Enqueue(Local(), src.Str(), Remote(fs, "host"),
                               "/remote/a.txt", {1024});
    exec.RunAll();

    REQUIRE_FALSE(prompt.asked);
    REQUIRE_FALSE(q.LastPreflight()->forecast.destination.known);
    REQUIRE(q.FindJob(id)->state == JobState::Active);
}

TEST_CASE("given a large copy to a server that cannot report free space then the user is told it was not checked") {
    // The reported failure, end to end: a multi-terabyte copy went to a
    // destination nobody could measure and started without a word.
    TempFile src("unsupported_large");
    FakeRemoteFileSystem fs;
    fs.spaceUnsupported = true;

    ManualExecutor exec;
    RecordingPrompt prompt;
    prompt.answer = false;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    const JobId id = q.Enqueue(Local(), src.Str(), Remote(fs, "host"),
                               "/remote/huge.bin", {4ULL << 40});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.DestinationUnverifiable());
    // Told it could not be checked, not told it will not fit.
    REQUIRE_FALSE(prompt.seen->forecast.Short());
    REQUIRE(q.FindJob(id)->state == JobState::Cancelled);
}

TEST_CASE("given a destination path that does not exist yet then an ancestor is asked instead") {
    // The batch is bound for directories the walk created moments earlier. A
    // query landing on one that is not there must not condemn the whole volume
    // as unmeasurable — an ancestor is on the same volume by definition.
    TempFile src("ancestor");
    FakeRemoteFileSystem fs;
    // Only the root of the destination can answer; the subdirectory cannot.
    fs.spaceByPath["/backup"] = Volume(1000);
    fs.statErrors.clear();

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    // Deliberately not seeded into spaceByPath, so the fake reports it as
    // missing rather than falling back to a default.
    fs.spaceMissing.insert("/backup/alpha");

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/backup/alpha/f", {100});
    exec.RunAll();

    REQUIRE(fs.spaceCalls.size() == 2);
    REQUIRE(fs.spaceCalls[0] == "/backup/alpha");
    REQUIRE(fs.spaceCalls[1] == "/backup");
    REQUIRE(q.LastPreflight()->forecast.destination.known);
    REQUIRE(q.LastPreflight()->forecast.destination.availableBytes == 1000);
}

TEST_CASE("given a server with no statvfs at all then the ancestry is not climbed") {
    // Unsupported is the server saying it has no statvfs, so every ancestor
    // would answer identically. Climbing would spend round trips to learn
    // nothing.
    TempFile src("no_climb");
    FakeRemoteFileSystem fs;
    fs.spaceUnsupported = true;

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/a/b/c/d/e/f.txt", {1024});
    exec.RunAll();

    REQUIRE(fs.spaceCalls.size() == 1);
}

TEST_CASE("given no warning prompt installed when a batch will not fit then it proceeds anyway") {
    // The opposite default to the conflict prompt, deliberately. An unanswerable
    // conflict risks destroying an existing file, so it does nothing; an
    // unanswerable space warning risks only a transfer that fails and can be
    // retried, whereas silently refusing the user's work is not recoverable by
    // anything they can see.
    TempFile src("noprompt");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1)));

    const JobId id = q.Enqueue(Local(), src.Str(), Remote(fs, "host"),
                               "/remote/a.txt", {5000});
    exec.RunAll();

    REQUIRE(q.FindJob(id)->state == JobState::Active);
}

TEST_CASE("given a read-only destination when queued then the user is warned though nothing is short") {
    TempFile src("readonly");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40, /*readOnly=*/true);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/a.txt", {10});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.destinationReadOnly);
    REQUIRE_FALSE(prompt.seen->forecast.Short());
}

// ---------------------------------------------------------------------------
// Batch scope
// ---------------------------------------------------------------------------

TEST_CASE("given a multi-file selection when checked then every file is counted once") {
    // A view queues a selection in a loop, so the batch is still growing while
    // the space query is in flight. Forecasting only the first item would
    // understate every multi-file copy.
    TempFile a("multi_a");
    TempFile b("multi_b");
    TempFile c("multi_c");

    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1000);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.Enqueue(Local(), a.Str(), Remote(fs, "host"), "/remote/a", {500});
    q.Enqueue(Local(), b.Str(), Remote(fs, "host"), "/remote/b", {500});
    q.Enqueue(Local(), c.Str(), Remote(fs, "host"), "/remote/c", {500});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.destination.requiredBytes == 1500);
    REQUIRE(prompt.seen->forecast.destination.ShortfallBytes() == 500);
    // One query for the batch, not one per file.
    REQUIRE(fs.spaceCalls.size() == 1);
}

TEST_CASE("given a directory copy when queued then nothing starts until the walk has finished") {
    // The queue's contract used to be that transfers begin immediately, which
    // made a pre-flight check impossible on a tree: the bytes were already
    // moving before the batch was known.
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40);
    fs.AddDirectory("/src", "src");
    fs.AddFile("/src/one", "one", 100, "/src");
    fs.AddFile("/src/two", "two", 200, "/src");

    FakeRemoteFileSystem dest;
    dest.defaultSpace = Volume(1ULL << 40);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    bool expanded = false;
    q.EnqueueTree(Remote(fs, "a"), "/src", Remote(dest, "b"), "/dst",
                  [&expanded](term::transport::FsError) { expanded = true; });
    exec.RunAll();

    REQUIRE(expanded);
    REQUIRE(q.LastPreflight().has_value());
    // Both leaves were in the batch the check saw.
    REQUIRE(q.LastPreflight()->forecast.destination.requiredBytes == 300);
}

TEST_CASE("given a second batch after the first completes when queued then it is checked afresh") {
    TempFile src("rearm");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/a", {10});
    exec.RunAll();
    fs.CompleteActive();
    exec.RunAll();
    REQUIRE(q.IsIdle());

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/b", {10});
    exec.RunAll();

    // A verdict about files that have already moved must not be inherited by
    // the next batch.
    REQUIRE(fs.spaceCalls.size() == 2);
}

// ---------------------------------------------------------------------------
// A verdict covers only the batch it measured
// ---------------------------------------------------------------------------

TEST_CASE("given a second batch queued while the first is running then it is checked on its own") {
    // The regression this exists for: a cleared batch left the queue in
    // "cleared" and any work arriving before it finished inherited that verdict
    // without ever being weighed. A 12 TB copy queued behind a small one went
    // through unwarned onto a volume with a fraction of the room.
    TempFile small("second_batch");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(120);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    const JobId first = q.Enqueue(Local(), small.Str(), Remote(fs, "host"),
                                  "/remote/small", {10});
    exec.RunAll();
    REQUIRE(q.FindJob(first)->state == JobState::Active);
    REQUIRE_FALSE(prompt.asked);

    // Still moving bytes when the large batch arrives.
    q.Enqueue(Local(), small.Str(), Remote(fs, "host"), "/remote/huge", {5000});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.destination.Short());
    REQUIRE(fs.spaceCalls.size() == 2);
}

TEST_CASE("given a batch already cleared when it drains then it is not re-checked per job") {
    // The other half of the watermark: jobs the verdict already covers must not
    // each trigger a fresh round trip as they come up.
    TempFile src("no_recheck");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/a", {10});
    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/b", {10});
    q.Enqueue(Local(), src.Str(), Remote(fs, "host"), "/remote/c", {10});
    exec.RunAll();

    for (int i = 0; i < 3; ++i) {
        fs.CompleteActive();
        exec.RunAll();
    }

    REQUIRE(q.IsIdle());
    REQUIRE(fs.spaceCalls.size() == 1);
}

TEST_CASE("given a directory whose parent is a symlink when copied then its contents are still counted") {
    // The reported case pointed at symlinks. A real directory selected inside a
    // symlinked parent is an ordinary directory: the link is in the path, not
    // in the entry, so the walk descends it and every leaf is weighed.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(120);

    src.AddDirectory("/link/real", "real", "/link");
    src.AddFile("/link/real/big.bin", "big.bin", 5000, "/link/real");

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    TransferItem item;
    item.path  = "/link/real";
    item.name  = "real";
    item.isDir = true;         // the entry itself is not a link
    q.EnqueueItem(Remote(src, "a"), item, Remote(dst, "b"), "/backup");
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.destination.requiredBytes == 5000);
}

TEST_CASE("given a selected entry that is itself a symlink then it is reproduced, not walked") {
    // Worth pinning because it looks like the bug and is not one: a link to a
    // directory is copied as a link, moves no bytes, and so is correctly
    // charged nothing. A selection of these transfers nothing at all.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(120);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    TransferItem item;
    item.path      = "/data/linked";
    item.name      = "linked";
    item.isSymlink = true;
    item.isDir     = true;     // resolved target is a directory
    q.EnqueueItem(Remote(src, "a"), item, Remote(dst, "b"), "/backup");
    exec.RunAll();

    REQUIRE_FALSE(prompt.asked);
    REQUIRE(q.LastPreflight()->forecast.destination.requiredBytes == 0);
    REQUIRE(src.listCalls.empty());   // never walked
}

// ---------------------------------------------------------------------------
// Pause
// ---------------------------------------------------------------------------

TEST_CASE("given a paused queue when a job finishes then the next one does not start") {
    TempFile src("pause");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    const JobId first  = q.Enqueue(Local(), src.Str(), Remote(fs, "h"), "/r/a", {10});
    const JobId second = q.Enqueue(Local(), src.Str(), Remote(fs, "h"), "/r/b", {10});
    exec.RunAll();
    REQUIRE(q.FindJob(first)->state == JobState::Active);

    q.Pause();
    REQUIRE(q.IsPaused());

    // The file already moving is deliberately left to land: there is no resume,
    // so stopping it would throw away the bytes it has written.
    REQUIRE(q.FindJob(first)->state == JobState::Active);

    fs.CompleteActive();
    exec.RunAll();

    REQUIRE(q.FindJob(first)->state == JobState::Completed);
    REQUIRE(q.FindJob(second)->state == JobState::Queued);
    REQUIRE(fs.ActiveCount() == 0);
}

TEST_CASE("given a paused queue when resumed then the remaining work runs") {
    TempFile src("resume");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(1ULL << 40);

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.Enqueue(Local(), src.Str(), Remote(fs, "h"), "/r/a", {10});
    const JobId second = q.Enqueue(Local(), src.Str(), Remote(fs, "h"), "/r/b", {10});
    exec.RunAll();

    q.Pause();
    fs.CompleteActive();
    exec.RunAll();
    REQUIRE(q.FindJob(second)->state == JobState::Queued);

    q.Resume();
    exec.RunAll();

    REQUIRE_FALSE(q.IsPaused());
    REQUIRE(q.FindJob(second)->state == JobState::Active);
}

TEST_CASE("given work queued while paused when resumed then it is still checked for space") {
    // Pausing must not become a way round the pre-flight: the check belongs to
    // the batch, not to the moment the user happened to press a button.
    TempFile src("pause_check");
    FakeRemoteFileSystem fs;
    fs.defaultSpace = Volume(100);

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.Pause();
    q.Enqueue(Local(), src.Str(), Remote(fs, "h"), "/r/huge", {5000});
    exec.RunAll();
    q.Resume();
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->forecast.destination.Short());
}

TEST_CASE("given a queue that was never started when paused then pausing is harmless") {
    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.Pause();
    q.Pause();      // idempotent
    q.Resume();
    q.Resume();     // resuming a running queue is a no-op

    REQUIRE_FALSE(q.IsPaused());
    REQUIRE(q.IsIdle());
}

// ---------------------------------------------------------------------------
// One unreadable directory must not truncate the batch
// ---------------------------------------------------------------------------

TEST_CASE("given an unreadable directory mid-tree when walked then the rest is still enumerated") {
    // The regression this exists for. A single directory that failed to list
    // ended the whole walk, and because the walk is breadth-first everything
    // still on the frontier went with it. A 12 TB selection was queued as 4 TB
    // and copied without a word.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);

    src.AddDirectory("/data", "data");
    for (const char* name : {"alpha", "beta", "gamma"}) {
        const std::string dir = std::string("/data/") + name;
        src.AddDirectory(dir, name, "/data");
        src.AddFile(dir + "/f.bin", "f.bin", 1000, dir);
    }
    // The middle one refuses, as a directory with awkward permissions would.
    src.listErrors["/data/beta"] =
        term::transport::FsError::Make(term::transport::FsErrorCode::PermissionDenied,
                                       "Permission denied");

    ManualExecutor exec;
    RecordingPrompt prompt;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    term::transport::FsError outcome;
    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup",
                  [&outcome](term::transport::FsError e) { outcome = std::move(e); });
    exec.RunAll();

    // alpha and gamma survived; only beta's contents are missing.
    REQUIRE(q.LastPreflight()->forecast.destination.requiredBytes == 2000);
    // The walk still reports failure, so a caller cannot mistake this for a
    // clean run.
    REQUIRE(outcome.Failed());
    REQUIRE(q.LastPreflight()->unreadableDirectories == 1);
    REQUIRE(q.LastPreflight()->firstFailurePath == "/data/beta");
    REQUIRE(q.LastPreflight()->CopyWillBeIncomplete());
}

TEST_CASE("given an incomplete enumeration when the batch fits then the user is still warned") {
    // Space is not the only reason to stop and ask. A copy that will be missing
    // whole directories completes, reports success, and is indistinguishable
    // from a complete one afterwards — so it has to be said before it starts.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);   // ample room

    src.AddDirectory("/data", "data");
    src.AddDirectory("/data/sub", "sub", "/data");
    src.AddFile("/data/keep.bin", "keep.bin", 10, "/data");
    src.listErrors["/data/sub"] =
        term::transport::FsError::Make(term::transport::FsErrorCode::PermissionDenied,
                                       "Permission denied");

    ManualExecutor exec;
    RecordingPrompt prompt;
    prompt.answer = false;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    exec.RunAll();

    REQUIRE(prompt.asked);
    REQUIRE(prompt.seen->CopyWillBeIncomplete());
    REQUIRE_FALSE(prompt.seen->forecast.Concerning());   // space was never the problem
    REQUIRE(q.IsIdle());                                 // declined, so nothing moved
}

TEST_CASE("given a session that dies mid-walk then the walk stops rather than grinding on") {
    // The one failure that must still abandon everything: every remaining
    // directory would fail identically, so pressing on spends a doomed listing
    // per directory to arrive in the same place.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);

    src.AddDirectory("/data", "data");
    for (const char* name : {"alpha", "beta", "gamma"}) {
        const std::string dir = std::string("/data/") + name;
        src.AddDirectory(dir, name, "/data");
        src.listErrors[dir] =
            term::transport::FsError::Make(term::transport::FsErrorCode::NotConnected,
                                           "The session closed");
    }

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    exec.RunAll();

    // Listed /data and then the first child, and gave up rather than trying the
    // other two.
    REQUIRE(src.listCalls.size() == 2);
}

TEST_CASE("given a new batch after an incomplete one then the skipped tally starts clean") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);

    src.AddDirectory("/data", "data");
    src.AddDirectory("/data/sub", "sub", "/data");
    src.AddFile("/data/f.bin", "f.bin", 10, "/data");
    src.listErrors["/data/sub"] =
        term::transport::FsError::Make(term::transport::FsErrorCode::PermissionDenied,
                                       "Permission denied");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    exec.RunAll();
    REQUIRE(q.LastPreflight()->unreadableDirectories == 1);

    // Drain, then queue a clean second batch. Both endpoints have to be driven:
    // a server-to-server job's first leg is a download from the source, and its
    // second an upload to the destination. The cap turns a queue that will not
    // retire into a failure rather than a hung run.
    for (int i = 0; i < 100 && !q.IsIdle(); ++i) {
        src.CompleteActive();
        dst.CompleteActive();
        exec.RunAll();
    }
    REQUIRE(q.IsIdle());

    src.AddDirectory("/clean", "clean");
    src.AddFile("/clean/g.bin", "g.bin", 10, "/clean");
    q.EnqueueTree(Remote(src, "a"), "/clean", Remote(dst, "b"), "/backup2", {});
    exec.RunAll();

    REQUIRE(q.LastPreflight()->unreadableDirectories == 0);
    REQUIRE_FALSE(q.LastPreflight()->CopyWillBeIncomplete());
}

// ---------------------------------------------------------------------------
// Enumeration is serial, and writes nothing
// ---------------------------------------------------------------------------

TEST_CASE("given several trees queued at once when enumerated then only one listing is in flight") {
    // The regression this exists for. Each selected tree used to be walked by
    // its own continuation chain, so several listings ran at once on one SFTP
    // session — and libssh2 keeps readdir state per session, so two overlapping
    // listings trade answers. A directory's contents came back attached to
    // another directory's path, and the copy then asked the server for files
    // that had never existed.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);
    src.deferListings = true;

    for (const char* name : {"alpha", "beta", "gamma"}) {
        const std::string dir = std::string("/data/") + name;
        src.AddDirectory(dir, name, "/data");
        src.AddFile(dir + "/f.bin", "f.bin", 10, dir);
    }

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    for (const char* name : {"alpha", "beta", "gamma"}) {
        TransferItem item;
        item.path  = std::string("/data/") + name;
        item.name  = name;
        item.isDir = true;
        q.EnqueueItem(Remote(src, "a"), item, Remote(dst, "b"), "/backup");
    }
    exec.RunAll();

    // Three trees queued, exactly one listing outstanding.
    REQUIRE(src.pendingLists.size() == 1);
    REQUIRE(q.IsExpanding());

    // And it stays that way as each one is answered.
    while (src.CompleteOldestListing()) {
        exec.RunAll();
        REQUIRE(src.pendingLists.size() <= 1);
    }

    REQUIRE_FALSE(q.IsExpanding());
    // All three were still enumerated, in full.
    REQUIRE(q.LastPreflight()->forecast.destination.requiredBytes == 30);
}

TEST_CASE("given a tree being enumerated then nothing is created on the destination") {
    // Enumeration happens before the user has agreed to anything, so it is a
    // preview and must behave like one.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(100);   // short, so the batch will be questioned

    src.AddDirectory("/data", "data");
    src.AddDirectory("/data/sub", "sub", "/data");
    src.AddFile("/data/sub/big.bin", "big.bin", 5000, "/data/sub");

    ManualExecutor exec;
    RecordingPrompt prompt;
    prompt.answer = false;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));
    q.SetSpaceWarningPrompt(prompt.Handler());

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    exec.RunAll();

    REQUIRE(prompt.asked);
    // Declined: the destination must be exactly as it was found.
    REQUIRE(dst.mkdirCalls.empty());
    REQUIRE(dst.transfers.empty());
    REQUIRE(q.IsIdle());
}

TEST_CASE("given an approved copy when it starts then directories are made parents first") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);

    src.AddDirectory("/data", "data");
    src.AddDirectory("/data/sub", "sub", "/data");
    src.AddFile("/data/sub/f.bin", "f.bin", 10, "/data/sub");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    exec.RunAll();

    // Breadth-first discovery order is parent-before-child, which is exactly
    // the order these have to be made in — no sorting, no recursive mkdir.
    REQUIRE(dst.mkdirCalls == std::vector<std::string>{"/backup", "/backup/sub"});
}

TEST_CASE("given an empty directory when copied then it is still created") {
    // It has no file job to carry it, so nothing else would.
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);
    src.AddDirectory("/data", "data");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    exec.RunAll();

    REQUIRE(dst.mkdirCalls == std::vector<std::string>{"/backup"});
    REQUIRE(q.IsIdle());
}

TEST_CASE("given a cancelled batch when it had directories pending then none are made") {
    FakeRemoteFileSystem src;
    FakeRemoteFileSystem dst;
    dst.defaultSpace = Volume(1ULL << 40);

    src.AddDirectory("/data", "data");
    src.AddDirectory("/data/sub", "sub", "/data");
    src.AddFile("/data/sub/f.bin", "f.bin", 10, "/data/sub");

    ManualExecutor exec;
    TransferQueue q(exec.AsDispatcher(), Staging(Volume(1ULL << 40)));

    q.EnqueueTree(Remote(src, "a"), "/data", Remote(dst, "b"), "/backup", {});
    // Cancel while enumeration is still in flight, before Pump ever reaches the
    // directory list.
    q.CancelAll();
    exec.RunAll();

    REQUIRE(dst.mkdirCalls.empty());
}
