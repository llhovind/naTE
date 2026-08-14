#include <catch2/catch_test_macros.hpp>

#include "FakeRemoteFileSystem.h"
#include "fs/EditWorkspace.h"
#include "fs/RemoteEditManager.h"
#include "fs/SaveAnnouncePolicy.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace term::fs;
using term::transport::FsError;
using term::transport::FsErrorCode;
using testing::FakeRemoteFileSystem;
using testing::ManualExecutor;

namespace {

FsError Failure(const std::string& message)
{
    return FsError::Make(FsErrorCode::Protocol, message);
}

// One editor launch, as the manager asked for it.
struct EditorLaunch {
    std::string command;
    std::string path;
};

// A manager wired to a fake filesystem and a dispatcher the test drains itself,
// plus somewhere for everything it reports to land.
//
// Bundled because every test needs the same five things wired the same way, and
// the wiring is not what any of them is about.
struct EditFixture {
    FakeRemoteFileSystem       fs;
    ManualExecutor             exec;
    std::vector<EditorLaunch>  launches;
    std::vector<std::string>   saved;        // remote paths that landed
    std::vector<SaveFailure>   failures;
    std::optional<bool>        openOk;
    std::string                openError;

    RemoteEditManager mgr{
        exec.AsDispatcher(),
        [this](const std::string& command, const std::string& path) {
            launches.push_back({command, path});
        }};

    EditFixture()
    {
        mgr.SetOnFileSaved([this](term::transport::IRemoteFileSystem*,
                                  const std::string& path) {
            saved.push_back(path);
        });
        mgr.SetOnFileSaveFailed([this](const SaveFailure& failure) {
            failures.push_back(failure);
        });
    }

    EditEndpoint Endpoint() { return EditEndpoint{&fs, "user@host"}; }

    // Opens one edit and drains everything it set in motion. Returns without
    // asserting: what an open did is exactly what several tests are checking.
    void Open(const std::string& remotePath, const std::string& editor = "vi")
    {
        mgr.OpenRemoteFile(Endpoint(), remotePath, editor,
                           [this](bool ok, std::string err) {
                               openOk    = ok;
                               openError = std::move(err);
                           });
        exec.RunAll();
    }

    // The working copy of the single open edit.
    std::string LocalPath() const
    {
        const auto edits = mgr.ListActiveEdits();
        return edits.empty() ? std::string{} : edits.front().localPath;
    }
};

// Writes to a working copy and closes it, which is what the watch is waiting
// for (IN_CLOSE_WRITE), then waits for the watch thread to post the upload.
//
// Bounded rather than instant because the notification travels through the
// kernel and a real thread — the two things the rest of this suite does not
// have to think about. Returns false if nothing arrived in time.
bool SaveAndAwaitUpload(EditFixture& fx, const std::string& localPath,
                        const std::string& contents = "edited\n")
{
    { std::ofstream(localPath) << contents; }

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(5);
    while (clock::now() < deadline) {
        fx.exec.RunAll();
        if (fx.fs.ActiveCount() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// The announce policy
//
// The rule about what a user is told when a save does or does not reach the
// remote. A save that silently fails is the dangerous case — the editor has
// already reported a clean write — and a save that reports the same broken
// connection on every keystroke is the reason the rule is not simply "always
// tell them".
// ---------------------------------------------------------------------------

TEST_CASE("given an upload succeeds when it finishes then the save is announced")
{
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(FsError::Success(), false) == SaveAnnouncement::Saved);
}

TEST_CASE("given an upload fails when it finishes then the failure is announced")
{
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
}

TEST_CASE("given the same failure twice running when both finish then it is announced once")
{
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Nothing);
}

TEST_CASE("given a different failure after one already announced when it finishes then it is announced too")
{
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
    REQUIRE(policy.Decide(Failure("permission denied"), false) ==
            SaveAnnouncement::Failed);
}

TEST_CASE("given a failure then a success when both finish then reporting is re-armed")
{
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
    REQUIRE(policy.Decide(FsError::Success(), false) == SaveAnnouncement::Saved);
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
}

TEST_CASE("given another save is queued when an upload finishes then its outcome is not announced")
{
    SaveAnnouncePolicy policy;

    SECTION("a success in the middle of a burst")
    {
        REQUIRE(policy.Decide(FsError::Success(), true) == SaveAnnouncement::Nothing);
    }

    SECTION("a failure in the middle of a burst")
    {
        REQUIRE(policy.Decide(Failure("connection lost"), true) ==
                SaveAnnouncement::Nothing);
    }
}

TEST_CASE("given a superseded failure when the burst ends with the same failure then it is still announced")
{
    // The superseded outcome must leave no trace: were it remembered, the
    // failure the user actually ends up with would be swallowed as a repeat.
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(Failure("connection lost"), true) ==
            SaveAnnouncement::Nothing);
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
}

TEST_CASE("given a superseded success when the burst ends with a repeat failure then it is not re-announced")
{
    // The mirror of the above: a superseded success must not re-arm reporting
    // either, or a burst would turn one broken connection into two dialogs.
    SaveAnnouncePolicy policy;
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Failed);
    REQUIRE(policy.Decide(FsError::Success(), true) == SaveAnnouncement::Nothing);
    REQUIRE(policy.Decide(Failure("connection lost"), false) ==
            SaveAnnouncement::Nothing);
}

// ---------------------------------------------------------------------------
// Opening an edit
// ---------------------------------------------------------------------------

TEST_CASE("given a remote file when the edit is opened then it downloads to a working copy and launches the editor")
{
    EditFixture fx;
    fx.mgr.OpenRemoteFile(fx.Endpoint(), "/etc/hosts", "vi",
                          [&fx](bool ok, std::string err) {
                              fx.openOk = ok;
                              fx.openError = std::move(err);
                          });

    REQUIRE(fx.fs.ActiveCount() == 1);
    const auto* transfer = fx.fs.Active();
    REQUIRE_FALSE(transfer->isUpload);
    REQUIRE(transfer->source == "/etc/hosts");
    // No mode is asked for: this is a working copy for an editor, not a
    // faithful reproduction of the remote file.
    REQUIRE_FALSE(transfer->sourceMode.has_value());

    const std::string localPath = transfer->dest;
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    REQUIRE(fx.openOk == true);
    REQUIRE(fx.launches.size() == 1);
    REQUIRE(fx.launches.front().command == "vi");
    REQUIRE(fx.launches.front().path == localPath);
    REQUIRE(fx.mgr.HasActiveEdits());

    const auto edits = fx.mgr.ListActiveEdits();
    REQUIRE(edits.size() == 1);
    REQUIRE(edits.front().remotePath == "/etc/hosts");
    REQUIRE(edits.front().host == "user@host");

    fx.mgr.StopSession(localPath);
}

TEST_CASE("given a download fails when the edit is opened then the working copy is removed and no session is created")
{
    EditFixture fx;
    fx.mgr.OpenRemoteFile(fx.Endpoint(), "/etc/hosts", "vi",
                          [&fx](bool ok, std::string err) {
                              fx.openOk = ok;
                              fx.openError = std::move(err);
                          });

    REQUIRE(fx.fs.Active() != nullptr);
    const std::string localPath = fx.fs.Active()->dest;
    REQUIRE(std::filesystem::exists(
        std::filesystem::path(localPath).parent_path()));

    fx.fs.CompleteActive(Failure("no such file"));
    fx.exec.RunAll();

    REQUIRE(fx.openOk == false);
    REQUIRE(fx.openError == "no such file");
    REQUIRE_FALSE(fx.mgr.HasActiveEdits());
    REQUIRE(fx.launches.empty());
    REQUIRE_FALSE(std::filesystem::exists(
        std::filesystem::path(localPath).parent_path()));
}

TEST_CASE("given an endpoint with no filesystem when an edit is opened then it is refused")
{
    EditFixture fx;
    fx.mgr.OpenRemoteFile(EditEndpoint{}, "/etc/hosts", "vi",
                          [&fx](bool ok, std::string err) {
                              fx.openOk = ok;
                              fx.openError = std::move(err);
                          });
    fx.exec.RunAll();

    REQUIRE(fx.openOk == false);
    REQUIRE_FALSE(fx.openError.empty());
    REQUIRE_FALSE(fx.mgr.HasActiveEdits());
}

TEST_CASE("given a remote path naming no file when the edit is opened then it is refused")
{
    EditFixture fx;
    fx.Open("/");

    REQUIRE(fx.openOk == false);
    REQUIRE_FALSE(fx.openError.empty());
    REQUIRE(fx.fs.ActiveCount() == 0);
    REQUIRE_FALSE(fx.mgr.HasActiveEdits());
}

TEST_CASE("given the same remote file opened twice when both downloads finish then each edit has its own working copy")
{
    // Sharing one would mean the second download truncating a file the first
    // editor is still holding — which the watch on it would upload straight
    // back to the remote.
    EditFixture fx;

    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();
    const std::string first = fx.LocalPath();

    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    const auto edits = fx.mgr.ListActiveEdits();
    REQUIRE(edits.size() == 2);
    REQUIRE(edits[0].localPath != edits[1].localPath);
    // Both keep the remote basename: it is what the editor shows and what its
    // syntax highlighting keys off.
    REQUIRE(std::filesystem::path(edits[0].localPath).filename() == "hosts");
    REQUIRE(std::filesystem::path(edits[1].localPath).filename() == "hosts");

    fx.mgr.StopSession(edits[0].localPath);
    fx.mgr.StopSession(edits[1].localPath);
    (void)first;
}

// ---------------------------------------------------------------------------
// Ending an edit
// ---------------------------------------------------------------------------

TEST_CASE("given an open edit when it is finished then the working copy is deleted")
{
    EditFixture fx;
    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    const std::string localPath = fx.LocalPath();
    // The directory, not the file: the fake moves no bytes, so what the open
    // actually created on disk is the private directory holding the copy — and
    // that is what has to go.
    const auto workingDir = std::filesystem::path(localPath).parent_path();
    REQUIRE(std::filesystem::exists(workingDir));

    fx.mgr.StopSession(localPath);

    REQUIRE_FALSE(fx.mgr.HasActiveEdits());
    REQUIRE_FALSE(std::filesystem::exists(workingDir));
}

TEST_CASE("given edits on a filesystem when it goes away then they stop and their working copies are deleted")
{
    EditFixture fx;
    FakeRemoteFileSystem other;

    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();
    const auto doomed = std::filesystem::path(fx.LocalPath()).parent_path();

    fx.mgr.OpenRemoteFile(EditEndpoint{&other, "elsewhere"}, "/etc/motd", "vi",
                          [](bool, std::string) {});
    other.CompleteActive();
    fx.exec.RunAll();

    REQUIRE(fx.mgr.ListActiveEdits().size() == 2);

    fx.mgr.StopEditsForFilesystem(&fx.fs);

    const auto remaining = fx.mgr.ListActiveEdits();
    REQUIRE(remaining.size() == 1);
    REQUIRE(remaining.front().remotePath == "/etc/motd");
    REQUIRE_FALSE(std::filesystem::exists(doomed));
    REQUIRE(std::filesystem::exists(
        std::filesystem::path(remaining.front().localPath).parent_path()));

    fx.mgr.StopEditsForFilesystem(&other);
}

TEST_CASE("given no edit on a filesystem when it goes away then nothing is stopped")
{
    EditFixture fx;
    FakeRemoteFileSystem other;

    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    SECTION("a filesystem nothing was opened against")
    {
        fx.mgr.StopEditsForFilesystem(&other);
    }

    SECTION("no filesystem at all")
    {
        fx.mgr.StopEditsForFilesystem(nullptr);
    }

    REQUIRE(fx.mgr.ListActiveEdits().size() == 1);
    fx.mgr.StopEditsForFilesystem(&fx.fs);
}

// ---------------------------------------------------------------------------
// Saving
//
// These drive the real watch: the working copy is written on the test's thread
// and the upload comes back through the inotify loop, which is the only way to
// show that a save in an editor is what actually starts one.
// ---------------------------------------------------------------------------

TEST_CASE("given an open edit when the working copy is saved then it is uploaded to the remote path")
{
    EditFixture fx;
    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    const std::string localPath = fx.LocalPath();
    REQUIRE(SaveAndAwaitUpload(fx, localPath));

    const auto* upload = fx.fs.Active();
    REQUIRE(upload->isUpload);
    REQUIRE(upload->source == localPath);
    REQUIRE(upload->dest == "/etc/hosts");
    // No mode: this writes back over a file whose permissions are the user's
    // and must survive the save untouched.
    REQUIRE_FALSE(upload->sourceMode.has_value());

    fx.fs.CompleteActive();
    fx.exec.RunAll();

    REQUIRE(fx.saved == std::vector<std::string>{"/etc/hosts"});
    REQUIRE(fx.failures.empty());

    fx.mgr.StopSession(localPath);
}

TEST_CASE("given an upload fails when it finishes then the failure names the local copy holding the edits")
{
    EditFixture fx;
    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    const std::string localPath = fx.LocalPath();
    REQUIRE(SaveAndAwaitUpload(fx, localPath));

    fx.fs.CompleteActive(Failure("connection lost"));
    fx.exec.RunAll();

    REQUIRE(fx.saved.empty());
    REQUIRE(fx.failures.size() == 1);
    REQUIRE(fx.failures.front().fs == &fx.fs);
    REQUIRE(fx.failures.front().remotePath == "/etc/hosts");
    REQUIRE(fx.failures.front().localPath == localPath);
    REQUIRE(fx.failures.front().message == "connection lost");

    fx.mgr.StopSession(localPath);
}

TEST_CASE("given an edit is finished while an upload is in flight when it completes then nothing is reported")
{
    // The working copy is gone by then, so a dialog offering to retry from it
    // would be pointing at nothing.
    EditFixture fx;
    fx.Open("/etc/hosts");
    fx.fs.CompleteActive();
    fx.exec.RunAll();

    const std::string localPath = fx.LocalPath();
    REQUIRE(SaveAndAwaitUpload(fx, localPath));

    fx.mgr.StopSession(localPath);
    fx.fs.CompleteActive(Failure("connection lost"));
    fx.exec.RunAll();

    REQUIRE(fx.failures.empty());
    REQUIRE(fx.saved.empty());
}
