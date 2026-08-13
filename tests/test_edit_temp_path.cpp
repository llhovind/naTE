#include <catch2/catch_test_macros.hpp>

#include "fs/EditTempPath.h"

#include <filesystem>
#include <fstream>
#include <string>

using term::fs::MakeEditTempPath;
using term::fs::RemoveWorkingCopy;
using term::fs::kEditTempRoot;

namespace {

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

} // namespace

TEST_CASE("given an absolute remote file when building a temp path "
          "then the directory is a template under the edit root")
{
    const auto p = MakeEditTempPath("root@host", "/root/web_stats.sh");

    REQUIRE(StartsWith(p.dirTemplate, std::string(kEditTempRoot) + "/"));
    REQUIRE(EndsWith(p.dirTemplate, "/XXXXXX"));
    REQUIRE(p.fileName == "web_stats.sh");
}

TEST_CASE("given the same host and file twice when building a temp path "
          "then both are templates rather than a shared directory")
{
    // The uniqueness comes from mkdtemp, so what this pins is that the scheme
    // hands it somewhere to apply: two opens must not resolve to one directory
    // before the filesystem has had its say.
    const auto a = MakeEditTempPath("root@host", "/root/web_stats.sh");
    const auto b = MakeEditTempPath("root@host", "/root/web_stats.sh");

    REQUIRE(EndsWith(a.dirTemplate, "/XXXXXX"));
    REQUIRE(a.dirTemplate == b.dirTemplate);
    REQUIRE(a.fileName == b.fileName);
}

TEST_CASE("given a remote directory when building a temp path "
          "then it is flattened into a single component")
{
    const auto p = MakeEditTempPath("host", "/var/log/nginx/access.log");

    REQUIRE(p.dirTemplate ==
            std::string(kEditTempRoot) + "/host/%2Fvar%2Flog%2Fnginx/XXXXXX");
    REQUIRE(p.fileName == "access.log");
}

TEST_CASE("given a remote path containing dot-dot when building a temp path "
          "then it cannot escape the edit root")
{
    const auto p = MakeEditTempPath("host", "/../../etc/passwd");

    REQUIRE(StartsWith(p.dirTemplate, std::string(kEditTempRoot) + "/"));
    REQUIRE(p.dirTemplate.find("/../") == std::string::npos);
    REQUIRE(p.fileName == "passwd");
}

TEST_CASE("given a hostname containing a separator when building a temp path "
          "then the host is one component too")
{
    const auto p = MakeEditTempPath("user@host/weird", "/tmp/a.txt");

    REQUIRE(StartsWith(p.dirTemplate,
                       std::string(kEditTempRoot) + "/user@host%2Fweird/"));
}

TEST_CASE("given a relative remote path when building a temp path "
          "then no empty directory level is emitted")
{
    const auto p = MakeEditTempPath("host", "notes.txt");

    REQUIRE(p.dirTemplate == std::string(kEditTempRoot) + "/host/XXXXXX");
    REQUIRE(p.fileName == "notes.txt");
    REQUIRE(p.dirTemplate.find("//") == std::string::npos);
}

TEST_CASE("given a remote path with a trailing separator when building a temp path "
          "then the name before it is used")
{
    const auto p = MakeEditTempPath("host", "/root/web_stats.sh/");

    REQUIRE(p.fileName == "web_stats.sh");
    REQUIRE(p.dirTemplate == std::string(kEditTempRoot) + "/host/%2Froot/XXXXXX");
}

TEST_CASE("given a remote path naming no file when building a temp path "
          "then the filename is empty so the caller can refuse it")
{
    REQUIRE(MakeEditTempPath("host", "/").fileName.empty());
    REQUIRE(MakeEditTempPath("host", "").fileName.empty());
    REQUIRE(MakeEditTempPath("host", "///").fileName.empty());
}

TEST_CASE("given a working copy when removed "
          "then the file and the levels it emptied are gone")
{
    const std::filesystem::path root(kEditTempRoot);
    const auto file = root / "host-removal" / "%2Froot" / "abc123" / "web_stats.sh";
    MakeFile(file);
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
    const std::filesystem::path root(kEditTempRoot);
    const auto shared = root / "host-siblings" / "%2Froot";
    const auto mine   = shared / "aaaaaa" / "web_stats.sh";
    const auto theirs = shared / "bbbbbb" / "web_stats.sh";
    MakeFile(mine);
    MakeFile(theirs);

    RemoveWorkingCopy(mine.string());

    REQUIRE_FALSE(std::filesystem::exists(mine.parent_path()));
    REQUIRE(std::filesystem::exists(theirs));
    REQUIRE(std::filesystem::exists(shared));

    std::filesystem::remove_all(root / "host-siblings");
}

TEST_CASE("given a path outside the edit root when removed then nothing happens")
{
    const auto dir = std::filesystem::temp_directory_path() / "nate-edit-outsider";
    const auto file = dir / "keep.txt";
    MakeFile(file);

    RemoveWorkingCopy(file.string());
    RemoveWorkingCopy((std::filesystem::path(kEditTempRoot) / ".." / "keep.txt").string());

    REQUIRE(std::filesystem::exists(file));

    std::filesystem::remove_all(dir);
}

TEST_CASE("given the edit root itself when removed then the root survives")
{
    const std::filesystem::path root(kEditTempRoot);
    std::filesystem::create_directories(root);

    RemoveWorkingCopy(root.string());

    REQUIRE(std::filesystem::exists(root));
}
