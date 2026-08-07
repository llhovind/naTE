#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/RemoteDeleter.h"

#include <optional>

using namespace term::fs;
using term::transport::FsError;
using term::transport::FsErrorCode;
using testing::FakeRemoteFileSystem;
using testing::ManualExecutor;

namespace {

// /top
//   a.txt
//   sub/
//     b.txt
//     deep/
//       c.txt
void SeedTree(FakeRemoteFileSystem& fs)
{
    fs.AddDirectory("/top", "top");
    fs.AddFile("/top/a.txt", "a.txt", 10, "/top");
    fs.AddDirectory("/top/sub", "sub", "/top");
    fs.AddFile("/top/sub/b.txt", "b.txt", 20, "/top/sub");
    fs.AddDirectory("/top/sub/deep", "deep", "/top/sub");
    fs.AddFile("/top/sub/deep/c.txt", "c.txt", 30, "/top/sub/deep");
}

DeletePlan PlanFor(RemoteDeleter& deleter, ManualExecutor& exec,
                   const std::string& path, bool isDir, bool isSymlink = false)
{
    DeletePlan captured;
    deleter.Plan(path, isDir, isSymlink,
                 [&](DeletePlan p) { captured = std::move(p); });
    exec.RunAll();
    return captured;
}

std::vector<std::string> StepPaths(const DeletePlan& plan)
{
    std::vector<std::string> out;
    for (const auto& s : plan.steps) out.push_back(s.path);
    return out;
}

// True when every child appears before its parent, which is the whole
// correctness requirement for the ordering.
bool IsPostOrder(const DeletePlan& plan)
{
    for (size_t i = 0; i < plan.steps.size(); ++i)
        for (size_t j = i + 1; j < plan.steps.size(); ++j) {
            const std::string& earlier = plan.steps[i].path;
            const std::string& later   = plan.steps[j].path;
            // If `earlier` contains `later`, a parent preceded its child.
            if (later.size() > earlier.size() &&
                later.compare(0, earlier.size(), earlier) == 0 &&
                later[earlier.size()] == '/')
                return false;
        }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Planning
// ---------------------------------------------------------------------------

TEST_CASE("given a single file when planned then it is one unlink") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top/a.txt", false);

    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.steps[0].path == "/top/a.txt");
    REQUIRE_FALSE(plan.steps[0].isDir);
    REQUIRE(plan.fileCount == 1);
    REQUIRE(plan.dirCount == 0);
    REQUIRE(fs.listCalls.empty());       // a file needs no enumeration
}

TEST_CASE("given a directory tree when planned then children precede their parents") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top", true);

    REQUIRE(plan.steps.size() == 6);
    REQUIRE(IsPostOrder(plan));
    REQUIRE(plan.steps.back().path == "/top");   // the root goes last
    REQUIRE(plan.fileCount == 3);
    REQUIRE(plan.dirCount == 3);
    REQUIRE(plan.totalBytes == 60);
}

TEST_CASE("given an empty directory when planned then it is one rmdir") {
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/empty", "empty");
    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/empty", true);

    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.steps[0].isDir);
    REQUIRE(plan.dirCount == 1);
    REQUIRE(plan.fileCount == 0);
}

TEST_CASE("given a symlink when planned then it is unlinked rather than followed") {
    // Deleting a link to /etc must delete the link, not the contents of /etc.
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    fs.AddDirectory("/etc", "etc");
    fs.AddFile("/etc/passwd", "passwd", 1000, "/etc");
    fs.AddSymlink("/top/link", "link", "/top");

    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top/link", true, /*isSymlink=*/true);

    REQUIRE(plan.steps.size() == 1);
    REQUIRE(plan.steps[0].path == "/top/link");
    REQUIRE_FALSE(plan.steps[0].isDir);          // unlink, not rmdir
    REQUIRE(fs.listCalls.empty());
}

TEST_CASE("given a tree containing a symlink when planned then the link is a leaf") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    fs.AddDirectory("/elsewhere", "elsewhere");
    fs.AddFile("/elsewhere/victim.txt", "victim.txt", 99, "/elsewhere");
    fs.AddSymlink("/top/link", "link", "/top");

    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top", true);
    const auto paths = StepPaths(plan);

    REQUIRE(std::find(paths.begin(), paths.end(), "/top/link") != paths.end());
    // Nothing on the far side of the link may appear in the plan.
    REQUIRE(std::find(paths.begin(), paths.end(), "/elsewhere") == paths.end());
    REQUIRE(std::find(paths.begin(), paths.end(), "/elsewhere/victim.txt")
            == paths.end());
}

TEST_CASE("given an unreadable subdirectory when planned then the plan is partial and says so") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    fs.listErrors["/top/sub/deep"] =
        FsError::Make(FsErrorCode::PermissionDenied, "denied");

    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top", true);

    REQUIRE(plan.error.Failed());
    REQUIRE(plan.error.code == FsErrorCode::PermissionDenied);
    // What was readable is still enumerated, so the user sees a real count.
    const auto paths = StepPaths(plan);
    REQUIRE(std::find(paths.begin(), paths.end(), "/top/a.txt") != paths.end());
    REQUIRE(std::find(paths.begin(), paths.end(), "/top/sub/b.txt") != paths.end());
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

TEST_CASE("given a plan when executed then removals happen in plan order") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top", true);

    std::optional<FsError> result;
    d.Execute(plan, nullptr, [&](FsError err) { result = std::move(err); });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->Ok());
    REQUIRE(fs.removeCalls == StepPaths(plan));
    REQUIRE(fs.removeCalls.back() == "/top");
}

TEST_CASE("given a plan when executed then progress counts every completed step") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    const DeletePlan plan = PlanFor(d, exec, "/top", true);

    std::vector<size_t> completed;
    size_t reportedTotal = 0;
    d.Execute(plan,
              [&](size_t done, size_t total) {
                  completed.push_back(done);
                  reportedTotal = total;
              },
              nullptr);
    exec.RunAll();

    REQUIRE(reportedTotal == plan.steps.size());
    REQUIRE(completed.size() == plan.steps.size());
    REQUIRE(completed.back() == plan.steps.size());
}

TEST_CASE("given a removal that fails when executed then it stops at the failure") {
    // Continuing past a failed unlink would bury the real cause under a run of
    // "directory not empty" errors from every ancestor.
    struct FailingFs : FakeRemoteFileSystem {
        std::string failPath;
        void Remove(const std::string& path, bool isDir,
                    term::transport::DoneCallback onDone) override
        {
            if (path == failPath) {
                removeCalls.push_back(path);
                onDone(FsError::Make(FsErrorCode::PermissionDenied, "denied"));
                return;
            }
            FakeRemoteFileSystem::Remove(path, isDir, std::move(onDone));
        }
    };

    FailingFs fs;
    SeedTree(fs);
    fs.failPath = "/top/sub/deep/c.txt";

    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());
    const DeletePlan plan = PlanFor(d, exec, "/top", true);

    std::optional<FsError> result;
    d.Execute(plan, nullptr, [&](FsError err) { result = std::move(err); });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->code == FsErrorCode::PermissionDenied);
    // The deepest file is attempted first and fails, so nothing above it runs.
    REQUIRE(fs.removeCalls.size() < plan.steps.size());
    REQUIRE(fs.removeCalls.back() == "/top/sub/deep/c.txt");
}

TEST_CASE("given an empty plan when executed then it succeeds without touching the transport") {
    FakeRemoteFileSystem fs;
    ManualExecutor exec;
    RemoteDeleter d(fs, exec.AsDispatcher());

    std::optional<FsError> result;
    d.Execute(DeletePlan{}, nullptr, [&](FsError err) { result = std::move(err); });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->Ok());
    REQUIRE(fs.removeCalls.empty());
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

TEST_CASE("given an enumeration in flight when the deleter is destroyed then the late callback is inert") {
    FakeRemoteFileSystem fs;
    SeedTree(fs);
    ManualExecutor exec;

    {
        RemoteDeleter d(fs, exec.AsDispatcher());
        d.Plan("/top", true, false, [](DeletePlan) {});
        REQUIRE_FALSE(exec.Empty());
    }

    REQUIRE_NOTHROW(exec.RunAll());
}
