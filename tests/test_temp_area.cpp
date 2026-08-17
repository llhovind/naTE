#include <catch2/catch_test_macros.hpp>

#include "fs/TempArea.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using term::fs::EnsurePrivateDirectory;
using term::fs::OwnerPidOfTaggedName;

namespace {

// A path under the system temp directory that no other test uses, removed
// whole on the way in and the way out so each case starts from nothing.
struct Scratch {
    std::filesystem::path path;

    explicit Scratch(const std::string& tag)
        : path(std::filesystem::temp_directory_path() /
               ("nate_temparea_" + tag + "_" + std::to_string(::getpid())))
    {
        Clear();
    }
    ~Scratch() { Clear(); }

    void Clear() const
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::string Str() const { return path.string(); }
};

// The permission bits as the filesystem holds them.
mode_t ModeOf(const std::string& path)
{
    struct stat st{};
    REQUIRE(::lstat(path.c_str(), &st) == 0);
    return st.st_mode & 07777;
}

} // namespace

// ---------------------------------------------------------------------------
// Reading the owner out of a name
// ---------------------------------------------------------------------------

TEST_CASE("given a name tagged with a pid when read then the pid comes back") {
    REQUIRE(OwnerPidOfTaggedName("4242-abc123") == 4242);
    REQUIRE(OwnerPidOfTaggedName("7-1-99-data.tar") == 7);
    // A single digit and nothing after the separator is still a tagged name.
    REQUIRE(OwnerPidOfTaggedName("1-") == 1);
}

TEST_CASE("given a name this scheme never wrote when the owner is read then it is not claimed") {
    // Anything unrecognised belongs to someone else, and a sweep must not
    // delete what it cannot prove is its own.
    REQUIRE(OwnerPidOfTaggedName("vmlinuz")     == std::nullopt);
    REQUIRE(OwnerPidOfTaggedName("")            == std::nullopt);
    REQUIRE(OwnerPidOfTaggedName("-42-1-x")     == std::nullopt);   // no pid in front
    REQUIRE(OwnerPidOfTaggedName("abc-1-x")     == std::nullopt);
    REQUIRE(OwnerPidOfTaggedName("12a4-abc123") == std::nullopt);
    REQUIRE(OwnerPidOfTaggedName("%2Froot")     == std::nullopt);
    // Too long to be a pid this scheme ever wrote.
    REQUIRE(OwnerPidOfTaggedName("99999999999999999999-abc") == std::nullopt);
}

// ---------------------------------------------------------------------------
// The directory those names live in
// ---------------------------------------------------------------------------

TEST_CASE("given no directory when one is ensured then it is created private") {
    const Scratch dir("fresh");

    REQUIRE(EnsurePrivateDirectory(dir.Str()));
    REQUIRE(std::filesystem::is_directory(dir.path));
    // Exactly 0700, whatever the umask happens to be: the mode has no group or
    // other bits for a umask to remove.
    REQUIRE(ModeOf(dir.Str()) == 0700);
}

TEST_CASE("given the directory already exists when ensured again then it is accepted") {
    const Scratch dir("idempotent");

    REQUIRE(EnsurePrivateDirectory(dir.Str()));
    REQUIRE(EnsurePrivateDirectory(dir.Str()));
    REQUIRE(ModeOf(dir.Str()) == 0700);
}

TEST_CASE("given our own directory left world-readable when ensured then it is tightened") {
    // The upgrade path: a root created by a run that let the umask decide is
    // ours to repair rather than to refuse, or every user whose directory
    // predates this rule would find the feature broken.
    const Scratch dir("widen");
    REQUIRE(::mkdir(dir.Str().c_str(), 0755) == 0);
    REQUIRE(ModeOf(dir.Str()) == 0755);

    REQUIRE(EnsurePrivateDirectory(dir.Str()));
    REQUIRE(ModeOf(dir.Str()) == 0700);
}

TEST_CASE("given a symlink where the directory belongs then it is refused, not followed") {
    // The pre-creation attack this exists for: a temp path has a fixed name,
    // so any local user can win the race to put something there first. A
    // symlink must be caught rather than followed to wherever it points.
    const Scratch link("symlink");
    const Scratch elsewhere("symlink_target");
    REQUIRE(EnsurePrivateDirectory(elsewhere.Str()));
    REQUIRE(::symlink(elsewhere.Str().c_str(), link.Str().c_str()) == 0);

    REQUIRE_FALSE(EnsurePrivateDirectory(link.Str()));
    // And nothing was written through it.
    REQUIRE(std::filesystem::is_empty(elsewhere.path));
}

TEST_CASE("given a plain file where the directory belongs then it is refused") {
    const Scratch file("plainfile");
    { std::ofstream out(file.path); out << "not a directory"; }

    REQUIRE_FALSE(EnsurePrivateDirectory(file.Str()));
}

TEST_CASE("given a path that cannot be created then it is reported rather than assumed") {
    // A parent that does not exist: this creates one level, never a chain.
    const Scratch dir("nested");
    REQUIRE_FALSE(EnsurePrivateDirectory((dir.path / "a" / "b").string()));
}

// Not covered here: a directory owned by *another* user, which is the case the
// uid check exists for. Standing one up needs a second uid, so it needs a
// privileged test runner — the symlink case above exercises the same defence
// against the same attacker at the same moment, and is the one that does not.
