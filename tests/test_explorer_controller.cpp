#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/ExplorerController.h"

#include <optional>

using namespace term::fs;
using term::transport::FsError;
using term::transport::FsErrorCode;
using testing::FakeRemoteFileSystem;
using testing::ManualExecutor;

namespace {

struct RecordingListener : IExplorerListener {
    std::vector<bool>        loading;
    int                      contentsChanges = 0;
    std::vector<std::string> paths;

    void OnExplorerLoadingChanged(bool l) override { loading.push_back(l); }
    void OnExplorerContentsChanged() override      { ++contentsChanges; }
    void OnExplorerPathChanged(const std::string& p) override { paths.push_back(p); }
};

// A small tree: /etc contains hosts (file), nginx (dir) and link (symlink).
void SeedEtc(FakeRemoteFileSystem& fs)
{
    fs.AddDirectory("/etc", "etc");
    fs.AddFile("/etc/hosts", "hosts", 120, "/etc");
    fs.AddDirectory("/etc/nginx", "nginx", "/etc");
    fs.AddFile("/etc/nginx/nginx.conf", "nginx.conf", 2048, "/etc/nginx");
}

std::vector<std::string> VisibleNames(const ExplorerController& c)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < c.Model().VisibleCount(); ++i)
        out.push_back(c.Model().At(i).name);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Canonicalisation policy
// ---------------------------------------------------------------------------

TEST_CASE("given a clean absolute path when tested then no canonicalisation is needed") {
    REQUIRE_FALSE(ExplorerController::NeedsCanonicalisation("/etc/nginx"));
    REQUIRE_FALSE(ExplorerController::NeedsCanonicalisation("/"));
}

TEST_CASE("given a relative or unresolved path when tested then canonicalisation is needed") {
    REQUIRE(ExplorerController::NeedsCanonicalisation("."));
    REQUIRE(ExplorerController::NeedsCanonicalisation(""));
    REQUIRE(ExplorerController::NeedsCanonicalisation("etc/nginx"));
    REQUIRE(ExplorerController::NeedsCanonicalisation("~/projects"));
    REQUIRE(ExplorerController::NeedsCanonicalisation("/etc/../etc"));
    REQUIRE(ExplorerController::NeedsCanonicalisation("/etc//nginx"));
}

TEST_CASE("given a clean absolute path when navigated then the server is not asked to resolve it") {
    // Ordinary click-through browsing should cost one round trip, not two.
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    REQUIRE(fs.listCalls == std::vector<std::string>{"/etc"});
    REQUIRE(c.CurrentPath() == "/etc");
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST_CASE("given a directory when navigated then the model holds its contents") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    RecordingListener listener;
    ExplorerController c(fs, exec.AsDispatcher());
    c.SetListener(&listener);

    c.NavigateTo("/etc");
    REQUIRE(c.IsLoading());
    exec.RunAll();

    REQUIRE_FALSE(c.IsLoading());
    REQUIRE(VisibleNames(c) == std::vector<std::string>{"nginx", "hosts"});
    REQUIRE(listener.paths == std::vector<std::string>{"/etc"});
    REQUIRE(listener.contentsChanges == 1);
    REQUIRE(listener.loading == std::vector<bool>{true, false});
}

TEST_CASE("given a listing that fails when it returns then the path still commits and the error surfaces") {
    // The view has to be able to say *which* directory refused to be read.
    FakeRemoteFileSystem fs;
    fs.AddDirectory("/root", "root");
    fs.listErrors["/root"] = FsError::Make(FsErrorCode::PermissionDenied, "denied");

    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/root");
    exec.RunAll();

    REQUIRE(c.CurrentPath() == "/root");
    REQUIRE(c.Model().HasError());
    REQUIRE(c.Model().Error().code == FsErrorCode::PermissionDenied);
    REQUIRE_FALSE(c.IsLoading());
}

TEST_CASE("given a failing realpath when navigating then the literal path is still listed") {
    // Losing the ability to canonicalise should not make a directory
    // unreachable when it can be listed perfectly well.
    struct RealPathFails : FakeRemoteFileSystem {
        void RealPath(const std::string&, term::transport::PathCallback onDone) override
        {
            onDone({}, FsError::Make(FsErrorCode::PermissionDenied, "denied"));
        }
    };

    RealPathFails fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc/../etc");     // forces the realpath path
    exec.RunAll();

    REQUIRE(fs.listCalls == std::vector<std::string>{"/etc/../etc"});
}

// ---------------------------------------------------------------------------
// Stale responses
// ---------------------------------------------------------------------------

TEST_CASE("given a superseded navigation when its listing arrives late then it is discarded") {
    // Without a generation guard a slow listing could land after a newer one
    // and drop the user into a directory they had already navigated away from.
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");          // response queued, not yet delivered
    c.NavigateTo("/etc/nginx");    // supersedes it
    exec.RunAll();                 // both responses now delivered, in order

    REQUIRE(c.CurrentPath() == "/etc/nginx");
    REQUIRE(VisibleNames(c) == std::vector<std::string>{"nginx.conf"});
}

TEST_CASE("given a superseded navigation when it is discarded then history records only the survivor") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    c.NavigateTo("/etc/nginx");
    exec.RunAll();

    REQUIRE_FALSE(c.CanGoBack());
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

TEST_CASE("given a sequence of navigations when going back then earlier directories return") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();
    REQUIRE_FALSE(c.CanGoBack());

    c.NavigateTo("/etc/nginx");
    exec.RunAll();
    REQUIRE(c.CanGoBack());
    REQUIRE_FALSE(c.CanGoForward());

    c.GoBack();
    exec.RunAll();
    REQUIRE(c.CurrentPath() == "/etc");
    REQUIRE(c.CanGoForward());
    REQUIRE_FALSE(c.CanGoBack());

    c.GoForward();
    exec.RunAll();
    REQUIRE(c.CurrentPath() == "/etc/nginx");
    REQUIRE_FALSE(c.CanGoForward());
}

TEST_CASE("given a step back when a new directory is entered then the forward history is discarded") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    fs.AddDirectory("/var", "var");
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");        exec.RunAll();
    c.NavigateTo("/etc/nginx");  exec.RunAll();
    c.GoBack();                  exec.RunAll();
    REQUIRE(c.CanGoForward());

    c.NavigateTo("/var");        exec.RunAll();
    REQUIRE_FALSE(c.CanGoForward());
    REQUIRE(c.CanGoBack());

    c.GoBack();                  exec.RunAll();
    REQUIRE(c.CurrentPath() == "/etc");
}

TEST_CASE("given a refresh when it completes then history is unchanged") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");        exec.RunAll();
    c.NavigateTo("/etc/nginx");  exec.RunAll();
    c.Refresh();                 exec.RunAll();

    REQUIRE(c.CurrentPath() == "/etc/nginx");
    REQUIRE(c.CanGoBack());
    REQUIRE_FALSE(c.CanGoForward());

    c.GoBack();                  exec.RunAll();
    REQUIRE(c.CurrentPath() == "/etc");
}

TEST_CASE("given a nested directory when navigating up then the parent is entered") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc/nginx");  exec.RunAll();
    c.NavigateUp();              exec.RunAll();

    REQUIRE(c.CurrentPath() == "/etc");
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

TEST_CASE("given a directory row when activated then the explorer enters it") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();

    std::optional<ActivationResult> result;
    c.Activate(c.Model().IndexOfName("nginx"),
               [&](ActivationResult r, std::string, FsError) { result = r; });
    exec.RunAll();

    REQUIRE(result == ActivationResult::Navigated);
    REQUIRE(c.CurrentPath() == "/etc/nginx");
}

TEST_CASE("given a file row when activated then its full path is reported") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();

    std::optional<ActivationResult> result;
    std::string path;
    c.Activate(c.Model().IndexOfName("hosts"),
               [&](ActivationResult r, std::string p, FsError) {
                   result = r; path = std::move(p);
               });
    exec.RunAll();

    REQUIRE(result == ActivationResult::IsFile);
    REQUIRE(path == "/etc/hosts");
    REQUIRE(c.CurrentPath() == "/etc");      // unchanged
}

TEST_CASE("given a symlink to a directory when activated then it is entered") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    fs.AddSymlink("/etc/alt", "alt", "/etc");
    // Stat follows the link, so seeding the link path as a directory is what
    // the server would report.
    fs.existing["/etc/alt"].isDir = true;
    fs.existing["/etc/alt"].mode  = 0040755;
    fs.listings["/etc/alt"] = {};

    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();

    std::optional<ActivationResult> result;
    c.Activate(c.Model().IndexOfName("alt"),
               [&](ActivationResult r, std::string, FsError) { result = r; });
    exec.RunAll();

    REQUIRE(result == ActivationResult::Navigated);
    REQUIRE(c.CurrentPath() == "/etc/alt");
}

TEST_CASE("given a symlink to a file when activated then it is reported as a file") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    fs.AddSymlink("/etc/resolv.conf", "resolv.conf", "/etc");
    fs.existing["/etc/resolv.conf"].isDir = false;
    fs.existing["/etc/resolv.conf"].mode  = 0100644;

    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();

    std::optional<ActivationResult> result;
    c.Activate(c.Model().IndexOfName("resolv.conf"),
               [&](ActivationResult r, std::string, FsError) { result = r; });
    exec.RunAll();

    REQUIRE(result == ActivationResult::IsFile);
}

TEST_CASE("given a broken symlink when activated then the failure is reported") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    // Present in the listing but with no stat target — a dangling link.
    term::transport::FileInfo link;
    link.name      = "dangling";
    link.mode      = 0120777;
    link.isSymlink = true;
    fs.listings["/etc"].push_back(link);

    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();

    std::optional<ActivationResult> result;
    FsError err;
    c.Activate(c.Model().IndexOfName("dangling"),
               [&](ActivationResult r, std::string, FsError e) {
                   result = r; err = std::move(e);
               });
    exec.RunAll();

    REQUIRE(result == ActivationResult::Failed);
    REQUIRE(err.code == FsErrorCode::NoSuchFile);
    REQUIRE(c.CurrentPath() == "/etc");
}

TEST_CASE("given an out of range row when activated then it fails without touching the transport") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();
    fs.statCalls.clear();

    std::optional<ActivationResult> result;
    c.Activate(99, [&](ActivationResult r, std::string, FsError) { result = r; });
    exec.RunAll();

    REQUIRE(result == ActivationResult::Failed);
    REQUIRE(fs.statCalls.empty());
}

// ---------------------------------------------------------------------------
// Filtering through the model
// ---------------------------------------------------------------------------

TEST_CASE("given a filtered model when a row path is requested then it joins the current directory") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());
    c.NavigateTo("/etc"); exec.RunAll();

    const size_t row = c.Model().IndexOfName("hosts");
    REQUIRE(c.PathOf(row) == "/etc/hosts");
    REQUIRE(c.PathOf(99).empty());
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

TEST_CASE("given a navigation in flight when the controller is destroyed then the late callback is inert") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;

    {
        ExplorerController c(fs, exec.AsDispatcher());
        c.NavigateTo("/etc");
        REQUIRE_FALSE(exec.Empty());
    }   // destroyed with the listing response still queued

    REQUIRE_NOTHROW(exec.RunAll());
}

// ---------------------------------------------------------------------------
// Writes
//
// These moved out of the wx pane so they could be tested at all. The rename
// path in particular carries an invariant that fails silently when it is wrong:
// POSIX rename() replaces its destination without a word.
// ---------------------------------------------------------------------------

TEST_CASE("given a name the server would reinterpret when validated then it is rejected") {
    using term::transport::FsErrorCode;

    REQUIRE(ExplorerController::ValidateLeafName("notes.txt").Ok());
    REQUIRE(ExplorerController::ValidateLeafName(".bashrc").Ok());
    REQUIRE(ExplorerController::ValidateLeafName("with space").Ok());

    for (const char* bad : {"", "a/b", "/", ".", ".."}) {
        const auto err = ExplorerController::ValidateLeafName(bad);
        REQUIRE(err.Failed());
        REQUIRE(err.code == FsErrorCode::InvalidName);
        REQUIRE_FALSE(err.message.empty());
    }
}

TEST_CASE("given an invalid name when a directory is created then the server is never asked") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    std::optional<term::transport::FsError> result;
    c.CreateDirectory("../escape", [&](term::transport::FsError err) {
        result = std::move(err);
    });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->code == term::transport::FsErrorCode::InvalidName);
    REQUIRE(fs.mkdirCalls.empty());
}

TEST_CASE("given a directory being shown when a folder is created then it lands in that directory") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    std::optional<term::transport::FsError> result;
    c.CreateDirectory("nginx.d", [&](term::transport::FsError err) {
        result = std::move(err);
    });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->Ok());
    REQUIRE(fs.mkdirCalls == std::vector<std::string>{"/etc/nginx.d"});
}

TEST_CASE("given a free destination when an entry is renamed then it is renamed in place") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    std::optional<term::transport::FsError> result;
    c.RenameEntry("hosts", "hosts.bak", [&](term::transport::FsError err) {
        result = std::move(err);
    });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->Ok());
    REQUIRE(fs.renameCalls == std::vector<std::string>{"/etc/hosts -> /etc/hosts.bak"});
}

TEST_CASE("given an occupied destination when an entry is renamed then nothing is overwritten") {
    // The invariant this whole path exists for: POSIX rename() would replace
    // the existing file silently, so the check has to happen before the call.
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    fs.AddFile("/etc/hosts.bak", "hosts.bak", 10, "/etc");

    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    std::optional<term::transport::FsError> result;
    c.RenameEntry("hosts", "hosts.bak", [&](term::transport::FsError err) {
        result = std::move(err);
    });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->code == term::transport::FsErrorCode::AlreadyExists);
    REQUIRE(fs.renameCalls.empty());
}

TEST_CASE("given a navigation landing mid-rename then the rename stays in its own directory") {
    // The reason writes take names rather than paths. A view that read the
    // current directory after its dialog closed would turn this into a move.
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    fs.AddDirectory("/var", "var");
    fs.listings["/var"] = {};

    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    // Rename issued against /etc, then the user's pending navigation arrives
    // before the destination check comes back.
    c.RenameEntry("hosts", "hosts.bak", [](term::transport::FsError) {});
    c.NavigateTo("/var");
    exec.RunAll();

    REQUIRE(c.CurrentPath() == "/var");
    REQUIRE(fs.renameCalls == std::vector<std::string>{"/etc/hosts -> /etc/hosts.bak"});
}

TEST_CASE("given an unchanged name when an entry is renamed then no round trip is made") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();
    fs.statCalls.clear();

    std::optional<term::transport::FsError> result;
    c.RenameEntry("hosts", "hosts", [&](term::transport::FsError err) {
        result = std::move(err);
    });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->Ok());
    REQUIRE(fs.statCalls.empty());
    REQUIRE(fs.renameCalls.empty());
}

TEST_CASE("given a directory being shown when permissions are set then they apply to that entry") {
    FakeRemoteFileSystem fs;
    SeedEtc(fs);
    ManualExecutor exec;
    ExplorerController c(fs, exec.AsDispatcher());

    c.NavigateTo("/etc");
    exec.RunAll();

    std::optional<term::transport::FsError> result;
    c.SetEntryPermissions("hosts", 0640, [&](term::transport::FsError err) {
        result = std::move(err);
    });
    exec.RunAll();

    REQUIRE(result.has_value());
    REQUIRE(result->Ok());
    REQUIRE(fs.chmodCalls.size() == 1);
    REQUIRE(fs.chmodCalls[0].path == "/etc/hosts");
    REQUIRE(fs.chmodCalls[0].mode == 0640u);
}
