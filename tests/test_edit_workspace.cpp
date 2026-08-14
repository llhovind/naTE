#include <catch2/catch_test_macros.hpp>

#include "fs/EditWorkspace.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using term::fs::MakeWorkingCopyPath;
using term::fs::OwnerPidOfWorkingCopyDir;
using term::fs::PurgeOrphanedWorkingCopies;
using term::fs::RemoveWorkingCopy;
using term::fs::kEditWorkspaceRoot;

namespace {

constexpr int kOwnerPid = 4242;

bool StartsWith(const std::string& s, const std::string& prefix)
{
    return s.rfind(prefix, 0) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Creates path and every directory above it, then writes an empty file there.
void MakeFile(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "";
}

// A working copy owned by pid, under a host directory of the test's choosing so
// concurrent test cases cannot collide.
std::filesystem::path MakeWorkingCopy(const std::string& host, int pid,
                                      const std::string& unique)
{
    const auto file = std::filesystem::path(kEditWorkspaceRoot) / host / "%2Froot" /
                      (std::to_string(pid) + "-" + unique) / "web_stats.sh";
    MakeFile(file);
    return file;
}

// Liveness stub: only the pids handed to it are running.
term::fs::OwnerIsLiveFn LiveSet(std::set<int> live)
{
    return [live = std::move(live)](int pid) { return live.count(pid) != 0; };
}

} // namespace

// ---------------------------------------------------------------------------
// Path scheme
// ---------------------------------------------------------------------------

TEST_CASE("given an absolute remote file when building a working copy path "
          "then the directory is a pid-tagged template under the root")
{
    const auto p = MakeWorkingCopyPath("root@host", "/root/web_stats.sh", kOwnerPid);

    REQUIRE(StartsWith(p.dirTemplate, std::string(kEditWorkspaceRoot) + "/"));
    REQUIRE(EndsWith(p.dirTemplate, "/4242-XXXXXX"));
    REQUIRE(p.fileName == "web_stats.sh");
}

TEST_CASE("given a remote directory when building a working copy path "
          "then it is flattened into a single component")
{
    const auto p = MakeWorkingCopyPath("host", "/var/log/nginx/access.log", kOwnerPid);

    REQUIRE(p.dirTemplate == std::string(kEditWorkspaceRoot) +
                                 "/host/%2Fvar%2Flog%2Fnginx/4242-XXXXXX");
    REQUIRE(p.fileName == "access.log");
}

TEST_CASE("given a remote path containing dot-dot when building a working copy path "
          "then it cannot escape the root")
{
    const auto p = MakeWorkingCopyPath("host", "/../../etc/passwd", kOwnerPid);

    REQUIRE(StartsWith(p.dirTemplate, std::string(kEditWorkspaceRoot) + "/"));
    REQUIRE(p.dirTemplate.find("/../") == std::string::npos);
    REQUIRE(p.fileName == "passwd");
}

TEST_CASE("given a hostname containing a separator when building a working copy path "
          "then the host is one component too")
{
    const auto p = MakeWorkingCopyPath("user@host/weird", "/tmp/a.txt", kOwnerPid);

    REQUIRE(StartsWith(p.dirTemplate,
                       std::string(kEditWorkspaceRoot) + "/user@host%2Fweird/"));
}

TEST_CASE("given a relative remote path when building a working copy path "
          "then no empty directory level is emitted")
{
    const auto p = MakeWorkingCopyPath("host", "notes.txt", kOwnerPid);

    REQUIRE(p.dirTemplate == std::string(kEditWorkspaceRoot) + "/host/4242-XXXXXX");
    REQUIRE(p.fileName == "notes.txt");
    REQUIRE(p.dirTemplate.find("//") == std::string::npos);
}

TEST_CASE("given a remote path with a trailing separator when building a working copy path "
          "then the name before it is used")
{
    const auto p = MakeWorkingCopyPath("host", "/root/web_stats.sh/", kOwnerPid);

    REQUIRE(p.fileName == "web_stats.sh");
    REQUIRE(p.dirTemplate ==
            std::string(kEditWorkspaceRoot) + "/host/%2Froot/4242-XXXXXX");
}

TEST_CASE("given a remote path naming no file when building a working copy path "
          "then the filename is empty so the caller can refuse it")
{
    REQUIRE(MakeWorkingCopyPath("host", "/", kOwnerPid).fileName.empty());
    REQUIRE(MakeWorkingCopyPath("host", "", kOwnerPid).fileName.empty());
    REQUIRE(MakeWorkingCopyPath("host", "///", kOwnerPid).fileName.empty());
}

// ---------------------------------------------------------------------------
// Owner pid round-trip
// ---------------------------------------------------------------------------

TEST_CASE("given a directory this module named when the owner is read "
          "then the pid comes back")
{
    // Round-tripped through the builder rather than a hand-written name, so the
    // two halves of the scheme cannot drift apart.
    const auto p = MakeWorkingCopyPath("host", "/root/f.txt", 12345);
    const auto leaf = std::filesystem::path(p.dirTemplate).filename().string();

    // mkdtemp will have replaced the XXXXXX by the time this is read back.
    const std::string realised = leaf.substr(0, leaf.size() - 6) + "a1b2c3";

    REQUIRE(OwnerPidOfWorkingCopyDir(realised) == 12345);
}

TEST_CASE("given a directory this module did not name when the owner is read "
          "then there is no pid to act on")
{
    REQUIRE_FALSE(OwnerPidOfWorkingCopyDir("scratch").has_value());
    REQUIRE_FALSE(OwnerPidOfWorkingCopyDir("-abc123").has_value());
    REQUIRE_FALSE(OwnerPidOfWorkingCopyDir("12a4-abc123").has_value());
    REQUIRE_FALSE(OwnerPidOfWorkingCopyDir("%2Froot").has_value());
    REQUIRE_FALSE(OwnerPidOfWorkingCopyDir("99999999999999999999-abc").has_value());
}

// ---------------------------------------------------------------------------
// Removal
// ---------------------------------------------------------------------------

TEST_CASE("given a working copy when removed "
          "then the file and the levels it emptied are gone")
{
    const std::filesystem::path root(kEditWorkspaceRoot);
    const auto file = MakeWorkingCopy("host-removal", kOwnerPid, "abc123");
    REQUIRE(std::filesystem::exists(file));

    RemoveWorkingCopy(file.string());

    REQUIRE_FALSE(std::filesystem::exists(file));
    REQUIRE_FALSE(std::filesystem::exists(root / "host-removal"));
    // The root survives: other sessions live under it.
    REQUIRE(std::filesystem::exists(root));

    std::filesystem::remove_all(root / "host-removal");
}

TEST_CASE("given a sibling working copy of the same file when one is removed "
          "then the walk stops at the level they share")
{
    // This is the case the per-open directory exists for: two edits of one
    // remote file. Removing either must leave the other's copy untouched.
    const std::filesystem::path root(kEditWorkspaceRoot);
    const auto mine   = MakeWorkingCopy("host-siblings", kOwnerPid, "aaaaaa");
    const auto theirs = MakeWorkingCopy("host-siblings", kOwnerPid, "bbbbbb");

    RemoveWorkingCopy(mine.string());

    REQUIRE_FALSE(std::filesystem::exists(mine.parent_path()));
    REQUIRE(std::filesystem::exists(theirs));
    REQUIRE(std::filesystem::exists(theirs.parent_path().parent_path()));

    std::filesystem::remove_all(root / "host-siblings");
}

TEST_CASE("given a path outside the root when removed then nothing happens")
{
    const auto dir  = std::filesystem::temp_directory_path() / "nate-edit-outsider";
    const auto file = dir / "keep.txt";
    MakeFile(file);

    RemoveWorkingCopy(file.string());
    RemoveWorkingCopy(
        (std::filesystem::path(kEditWorkspaceRoot) / ".." / "keep.txt").string());

    REQUIRE(std::filesystem::exists(file));

    std::filesystem::remove_all(dir);
}

TEST_CASE("given the root itself when removed then the root survives")
{
    const std::filesystem::path root(kEditWorkspaceRoot);
    std::filesystem::create_directories(root);

    RemoveWorkingCopy(root.string());

    REQUIRE(std::filesystem::exists(root));
}

// ---------------------------------------------------------------------------
// Orphan reclamation
// ---------------------------------------------------------------------------

TEST_CASE("given a working copy whose owner is gone when purging "
          "then it is reclaimed")
{
    const std::filesystem::path root(kEditWorkspaceRoot);
    const auto dead = MakeWorkingCopy("host-purge-dead", 1001, "aaaaaa");

    const size_t reclaimed = PurgeOrphanedWorkingCopies(LiveSet({}));

    REQUIRE(reclaimed >= 1);
    REQUIRE_FALSE(std::filesystem::exists(dead));
    REQUIRE_FALSE(std::filesystem::exists(root / "host-purge-dead"));

    std::filesystem::remove_all(root / "host-purge-dead");
}

TEST_CASE("given a working copy whose owner is still running when purging "
          "then it is left completely alone")
{
    // The multi-instance case: a second naTE starting up must not delete the
    // first one's working copies, which may hold unsaved edits.
    const std::filesystem::path root(kEditWorkspaceRoot);
    const auto live = MakeWorkingCopy("host-purge-live", 1002, "bbbbbb");
    const auto dead = MakeWorkingCopy("host-purge-live", 1003, "cccccc");

    PurgeOrphanedWorkingCopies(LiveSet({1002}));

    REQUIRE(std::filesystem::exists(live));
    REQUIRE_FALSE(std::filesystem::exists(dead));

    std::filesystem::remove_all(root / "host-purge-live");
}

TEST_CASE("given a directory this module did not create when purging "
          "then it is left alone")
{
    // Reclaiming only what it can prove is dead: an unrecognised directory
    // belongs to something else, and guessing would be how user data is lost.
    const std::filesystem::path root(kEditWorkspaceRoot);
    const auto stranger = root / "host-purge-stranger" / "someone-elses" / "notes.txt";
    MakeFile(stranger);

    PurgeOrphanedWorkingCopies(LiveSet({}));

    REQUIRE(std::filesystem::exists(stranger));

    std::filesystem::remove_all(root / "host-purge-stranger");
}

TEST_CASE("given nothing to reclaim when purging then the root is left standing")
{
    const std::filesystem::path root(kEditWorkspaceRoot);
    std::filesystem::create_directories(root);

    REQUIRE(PurgeOrphanedWorkingCopies(LiveSet({})) == 0);
    REQUIRE(std::filesystem::exists(root));
}
