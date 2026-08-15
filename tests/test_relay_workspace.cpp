#include <catch2/catch_test_macros.hpp>

#include "fs/RelayWorkspace.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace term::fs;

namespace {

// Writes a file directly into the relay root, bypassing MakeRelayPath, so a
// test can plant names the scheme would never produce.
void Plant(const std::string& name, const std::string& contents = "x")
{
    REQUIRE(EnsureRelayRoot());
    std::ofstream out(std::filesystem::path(RelayRoot()) / name);
    out << contents;
}

bool Exists(const std::string& name)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(RelayRoot()) / name, ec);
}

// Nothing of this run's own may be left behind between cases, and the root is
// shared with any real naTE on the same machine — so only names this test
// planted are removed.
void RemovePlanted(const std::initializer_list<std::string>& names)
{
    std::error_code ec;
    for (const auto& name : names)
        std::filesystem::remove(std::filesystem::path(RelayRoot()) / name, ec);
}

// A pid that cannot be running: the kernel's maximum is well below this, so no
// liveness test can mistake it for a live owner.
constexpr int kDeadPid = 0x7FFFFFF0;

} // namespace

// ---------------------------------------------------------------------------
// Naming
// ---------------------------------------------------------------------------

TEST_CASE("given a staging path when built then it carries the owner pid first") {
    const std::string path = MakeRelayPath("data.tar", 7, 4242, 99);
    const std::string leaf = std::filesystem::path(path).filename().string();

    REQUIRE(leaf.rfind("4242-", 0) == 0);
    REQUIRE(OwnerPidOfRelayFile(leaf) == 4242);
}

TEST_CASE("given two queues numbering jobs alike when paths are built then they differ") {
    // One process runs several transfer queues, each numbering its jobs from
    // one. Without the timestamp two windows relaying at once would collide on
    // a name and each truncate the other's file.
    const std::string a = MakeRelayPath("data.tar", 1, 4242, 100);
    const std::string b = MakeRelayPath("data.tar", 1, 4242, 101);

    REQUIRE(a != b);
}

TEST_CASE("given a remote name with separators when a staging path is built then it stays in the root") {
    // The leaf comes from a server and is bytes, not a name this may trust. A
    // path escaping the root is how a hostile listing would reach elsewhere.
    const std::string path = MakeRelayPath("../../etc/passwd", 1, 4242, 100);
    const std::string leaf = std::filesystem::path(path).filename().string();

    REQUIRE(std::filesystem::path(path).parent_path() == std::filesystem::path(RelayRoot()));

    // What makes it safe is that no separator survives: with the whole thing
    // reduced to one component there is nothing for ".." to traverse through.
    // Dots themselves stay legal — an extension is worth keeping — and a name
    // like "..-etc" is a perfectly ordinary file inside the root.
    REQUIRE(leaf.find('/') == std::string::npos);
    // The pid always leads, so the name can never come out as "." or "..".
    REQUIRE(leaf.rfind("4242-", 0) == 0);
}

TEST_CASE("given an overlong remote name when a staging path is built then the leaf is truncated") {
    const std::string huge(500, 'a');
    const std::string leaf =
        std::filesystem::path(MakeRelayPath(huge, 1, 4242, 100)).filename().string();

    // NAME_MAX is 255 on Linux, and the name also carries a pid, a job id and
    // a timestamp in front of the truncated leaf.
    REQUIRE(leaf.size() < 255);
}

TEST_CASE("given a name this module never wrote when the owner is read then it is not claimed") {
    // Anything unrecognised belongs to someone else, and a sweep must not
    // delete what it cannot prove is its own.
    REQUIRE(OwnerPidOfRelayFile("vmlinuz") == std::nullopt);
    REQUIRE(OwnerPidOfRelayFile("-42-1-x") == std::nullopt);
    REQUIRE(OwnerPidOfRelayFile("abc-1-x") == std::nullopt);
    REQUIRE(OwnerPidOfRelayFile("") == std::nullopt);
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------

TEST_CASE("given a staging file whose owner is gone when purged then it is reclaimed") {
    const std::string dead =
        std::filesystem::path(MakeRelayPath("big.iso", 1, kDeadPid, 1))
            .filename().string();
    Plant(dead);

    const size_t reclaimed = PurgeOrphanedRelayFiles(
        [](int pid) { return pid != kDeadPid; });

    REQUIRE(reclaimed >= 1);
    REQUIRE_FALSE(Exists(dead));
}

TEST_CASE("given a staging file whose owner is running when purged then it is left alone") {
    // naTE runs several instances. Deleting another one's staging file would
    // abort a transfer that is in progress right now.
    const std::string live =
        std::filesystem::path(MakeRelayPath("big.iso", 1, 4242, 1))
            .filename().string();
    Plant(live);

    PurgeOrphanedRelayFiles([](int) { return true; });

    REQUIRE(Exists(live));
    RemovePlanted({live});
}

TEST_CASE("given a file this module never wrote when purged then it survives") {
    Plant("not-ours.txt");

    PurgeOrphanedRelayFiles([](int) { return false; });

    REQUIRE(Exists("not-ours.txt"));
    RemovePlanted({"not-ours.txt"});
}

TEST_CASE("given a directory in the staging root when purged then it is not descended into") {
    REQUIRE(EnsureRelayRoot());
    const auto dir = std::filesystem::path(RelayRoot()) / "4242-1-1-adirectory";
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);

    PurgeOrphanedRelayFiles([](int) { return false; });

    REQUIRE(std::filesystem::exists(dir, ec));
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("given no staging root when purged then nothing happens") {
    // The ordinary state before this run's first server-to-server copy.
    std::error_code ec;
    std::filesystem::remove_all(RelayRoot(), ec);

    REQUIRE(PurgeOrphanedRelayFiles([](int) { return false; }) == 0);
}

// ---------------------------------------------------------------------------
// The root
// ---------------------------------------------------------------------------

TEST_CASE("given the staging root when created then it is reachable only by its owner") {
    std::error_code ec;
    std::filesystem::remove_all(RelayRoot(), ec);

    REQUIRE(EnsureRelayRoot());

    const auto perms = std::filesystem::status(RelayRoot(), ec).permissions();
    // A staging file carries the source's own permissions, but the names of the
    // files someone is moving between two servers are worth keeping private too.
    REQUIRE((perms & std::filesystem::perms::group_all) == std::filesystem::perms::none);
    REQUIRE((perms & std::filesystem::perms::others_all) == std::filesystem::perms::none);
}

TEST_CASE("given the staging root already present when ensured again then it succeeds") {
    REQUIRE(EnsureRelayRoot());
    REQUIRE(EnsureRelayRoot());   // every transfer after the first
}

TEST_CASE("given the staging volume when queried then it reports something usable") {
    REQUIRE(EnsureRelayRoot());

    const auto space = RelayVolumeSpace();
    REQUIRE(space.has_value());
    REQUIRE(space->totalBytes > 0);
}

TEST_CASE("given no staging root yet when the volume is queried then it still answers") {
    // The first server-to-server copy of a run has to be checkable, and at that
    // point the root has not been created — the temp directory above it is on
    // the same volume and answers identically.
    std::error_code ec;
    std::filesystem::remove_all(RelayRoot(), ec);

    REQUIRE(RelayVolumeSpace().has_value());
}
