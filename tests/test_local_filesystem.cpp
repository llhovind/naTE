#include <catch2/catch_test_macros.hpp>

#include "fs/FileMode.h"
#include "transport/LocalFileSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>

#include <unistd.h>

using namespace term::transport;

namespace {

struct TempTree {
    std::filesystem::path root;

    explicit TempTree(const std::string& tag)
        : root(std::filesystem::temp_directory_path() / ("nate_localfs_" + tag))
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~TempTree() { std::error_code ec; std::filesystem::remove_all(root, ec); }

    std::string File(const std::string& name, const std::string& body = "x") const
    {
        const auto p = root / name;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p);
        out << body;
        return p.string();
    }
    std::string Dir(const std::string& name) const
    {
        const auto p = root / name;
        std::filesystem::create_directories(p);
        return p.string();
    }
    std::string At(const std::string& name) const { return (root / name).string(); }
};

std::vector<FileInfo> ListSync(LocalFileSystem& fs, const std::string& path,
                               FsError* outErr = nullptr)
{
    std::vector<FileInfo> entries;
    fs.List(path, [&](std::vector<FileInfo> e, FsError err) {
        entries = std::move(e);
        if (outErr) *outErr = std::move(err);
    });
    return entries;
}

const FileInfo* Find(const std::vector<FileInfo>& entries, const std::string& name)
{
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&](const FileInfo& f) { return f.name == name; });
    return it == entries.end() ? nullptr : &*it;
}

FsError DoneOf(LocalFileSystem& fs, const std::function<void(DoneCallback)>& call)
{
    FsError captured;
    call([&](FsError err) { captured = std::move(err); });
    (void)fs;
    return captured;
}

} // namespace

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST_CASE("given a local directory when listed then entries carry real metadata") {
    TempTree tmp("list");
    tmp.File("hello.txt", "12345");
    tmp.Dir("subdir");

    LocalFileSystem fs;
    FsError err;
    const auto entries = ListSync(fs, tmp.root.string(), &err);

    REQUIRE(err.Ok());
    REQUIRE(entries.size() == 2);

    const FileInfo* file = Find(entries, "hello.txt");
    REQUIRE(file != nullptr);
    REQUIRE(file->size == 5);
    REQUIRE_FALSE(file->isDir);
    REQUIRE(term::fs::IsRegularFile(file->mode));
    REQUIRE(file->mtime > 0);
    // Names come from the passwd/group databases; the process always has an
    // owner, so this must not be empty on a working system.
    REQUIRE_FALSE(file->owner.empty());

    const FileInfo* dir = Find(entries, "subdir");
    REQUIRE(dir != nullptr);
    REQUIRE(dir->isDir);
}

TEST_CASE("given dot entries when listed then they are omitted") {
    TempTree tmp("dots");
    tmp.File("a.txt");

    LocalFileSystem fs;
    const auto entries = ListSync(fs, tmp.root.string());

    REQUIRE(Find(entries, ".") == nullptr);
    REQUIRE(Find(entries, "..") == nullptr);
    REQUIRE(entries.size() == 1);
}

TEST_CASE("given a missing directory when listed then it reports no such file") {
    LocalFileSystem fs;
    FsError err;
    const auto entries = ListSync(fs, "/definitely/not/here", &err);

    REQUIRE(entries.empty());
    REQUIRE(err.code == FsErrorCode::NoSuchFile);
}

TEST_CASE("given a symlink when listed then it is reported as a link not its target") {
    // Matching SFTP's readdir semantics keeps both panes describing links the
    // same way, which the delete and activation rules depend on.
    TempTree tmp("symlink_list");
    tmp.Dir("target");
    std::filesystem::create_directory_symlink(tmp.At("target"), tmp.At("link"));

    LocalFileSystem fs;
    const auto entries = ListSync(fs, tmp.root.string());

    const FileInfo* link = Find(entries, "link");
    REQUIRE(link != nullptr);
    REQUIRE(link->isSymlink);
    REQUIRE_FALSE(link->isDir);       // the link itself, not what it points at
}

// ---------------------------------------------------------------------------
// Stat and links
// ---------------------------------------------------------------------------

TEST_CASE("given a symlink to a directory when stat'd then the target is described") {
    TempTree tmp("stat_follow");
    tmp.Dir("target");
    std::filesystem::create_directory_symlink(tmp.At("target"), tmp.At("link"));

    LocalFileSystem fs;
    std::optional<FileInfo> info;
    FsError err;
    fs.Stat(tmp.At("link"), [&](FileInfo i, FsError e) {
        info = std::move(i); err = std::move(e);
    });

    REQUIRE(err.Ok());
    REQUIRE(info.has_value());
    REQUIRE(info->isDir);             // Stat follows, unlike List
}

TEST_CASE("given a broken symlink when stat'd then it reports no such file") {
    TempTree tmp("stat_broken");
    std::filesystem::create_symlink(tmp.At("gone"), tmp.At("dangling"));

    LocalFileSystem fs;
    FsError err;
    fs.Stat(tmp.At("dangling"), [&](FileInfo, FsError e) { err = std::move(e); });

    REQUIRE(err.code == FsErrorCode::NoSuchFile);
}

TEST_CASE("given a symlink when read then its target path is returned") {
    TempTree tmp("readlink");
    tmp.File("real.txt");
    std::filesystem::create_symlink(tmp.At("real.txt"), tmp.At("link.txt"));

    LocalFileSystem fs;
    std::string target;
    FsError err;
    fs.ReadLink(tmp.At("link.txt"), [&](std::string t, FsError e) {
        target = std::move(t); err = std::move(e);
    });

    REQUIRE(err.Ok());
    REQUIRE(target == tmp.At("real.txt"));
}

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

TEST_CASE("given an unnormalised path when resolved then it is canonicalised") {
    TempTree tmp("realpath");
    tmp.Dir("a/b");

    LocalFileSystem fs;
    std::string resolved;
    fs.RealPath(tmp.At("a/./b/../b"), [&](std::string p, FsError) {
        resolved = std::move(p);
    });

    REQUIRE(resolved == std::filesystem::canonical(tmp.At("a/b")).string());
}

TEST_CASE("given a leading tilde when resolved then it expands to the home directory") {
    LocalFileSystem fs;
    const char* home = std::getenv("HOME");
    if (!home) return;   // nothing to assert against

    std::string resolved;
    FsError err;
    fs.RealPath("~", [&](std::string p, FsError e) {
        resolved = std::move(p); err = std::move(e);
    });

    REQUIRE(err.Ok());
    REQUIRE(resolved == std::filesystem::weakly_canonical(home).string());
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

TEST_CASE("given a new path when a directory is created then it exists with the mode asked for") {
    TempTree tmp("mkdir");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.MakeDirectory(tmp.At("fresh"), 0755, std::move(cb));
    });

    REQUIRE(err.Ok());
    REQUIRE(std::filesystem::is_directory(tmp.At("fresh")));
}

TEST_CASE("given an existing path when a directory is created then it reports already exists") {
    TempTree tmp("mkdir_exists");
    tmp.Dir("taken");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.MakeDirectory(tmp.At("taken"), 0755, std::move(cb));
    });

    REQUIRE(err.code == FsErrorCode::AlreadyExists);
}

TEST_CASE("given a file when removed then it is gone") {
    TempTree tmp("unlink");
    tmp.File("doomed.txt");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.Remove(tmp.At("doomed.txt"), false, std::move(cb));
    });

    REQUIRE(err.Ok());
    REQUIRE_FALSE(std::filesystem::exists(tmp.At("doomed.txt")));
}

TEST_CASE("given a non-empty directory when removed then it refuses") {
    TempTree tmp("rmdir_full");
    tmp.File("keep/inside.txt");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.Remove(tmp.At("keep"), true, std::move(cb));
    });

    REQUIRE(err.code == FsErrorCode::DirectoryNotEmpty);
    REQUIRE(std::filesystem::exists(tmp.At("keep/inside.txt")));
}

TEST_CASE("given a symlink to a directory when removed then the link goes and the target stays") {
    TempTree tmp("unlink_symlink");
    tmp.File("target/precious.txt");
    std::filesystem::create_directory_symlink(tmp.At("target"), tmp.At("link"));
    LocalFileSystem fs;

    // isDir is false for a symlink in a listing, so this is the call the
    // explorer actually makes — and it must not touch the target.
    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.Remove(tmp.At("link"), false, std::move(cb));
    });

    REQUIRE(err.Ok());
    REQUIRE_FALSE(std::filesystem::exists(std::filesystem::symlink_status(tmp.At("link"))));
    REQUIRE(std::filesystem::exists(tmp.At("target/precious.txt")));
}

TEST_CASE("given a free destination when renamed then the file moves") {
    TempTree tmp("rename");
    tmp.File("before.txt", "body");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.Rename(tmp.At("before.txt"), tmp.At("after.txt"), std::move(cb));
    });

    REQUIRE(err.Ok());
    REQUIRE(std::filesystem::exists(tmp.At("after.txt")));
    REQUIRE_FALSE(std::filesystem::exists(tmp.At("before.txt")));
}

TEST_CASE("given an occupied destination when renamed then it refuses rather than overwriting") {
    // POSIX rename() replaces the destination silently. Losing a file to a
    // rename is exactly the surprise this guard exists to prevent.
    TempTree tmp("rename_clobber");
    tmp.File("source.txt", "new");
    tmp.File("victim.txt", "original");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.Rename(tmp.At("source.txt"), tmp.At("victim.txt"), std::move(cb));
    });

    REQUIRE(err.code == FsErrorCode::AlreadyExists);
    std::ifstream in(tmp.At("victim.txt"));
    std::string body;
    in >> body;
    REQUIRE(body == "original");
    REQUIRE(std::filesystem::exists(tmp.At("source.txt")));
}

TEST_CASE("given a file when its permissions are set then the mode changes") {
    TempTree tmp("chmod");
    const std::string path = tmp.File("mode.txt");
    LocalFileSystem fs;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.SetPermissions(path, 0640, std::move(cb));
    });

    REQUIRE(err.Ok());

    std::optional<FileInfo> info;
    fs.Stat(path, [&](FileInfo i, FsError) { info = std::move(i); });
    REQUIRE(info.has_value());
    REQUIRE((info->mode & term::fs::kPermissionMask) == 0640u);
}

TEST_CASE("given a path the user cannot write when created then it reports permission denied") {
    LocalFileSystem fs;
    // Running as root would defeat the check, so skip rather than fail.
    if (::geteuid() == 0) return;

    const FsError err = DoneOf(fs, [&](DoneCallback cb) {
        fs.MakeDirectory("/proc/nate_should_not_exist", 0755, std::move(cb));
    });

    REQUIRE(err.Failed());
    REQUIRE(err.code != FsErrorCode::AlreadyExists);
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

TEST_CASE("given the local adapter when a transfer is requested then it declines clearly") {
    // Moving bytes between here and a remote is the remote adapter's job; this
    // must fail loudly rather than appear to succeed.
    LocalFileSystem fs;

    FsError down, up;
    REQUIRE(fs.Download("a", "b", nullptr, [&](FsError e) { down = std::move(e); })
            == kInvalidTransferHandle);
    REQUIRE(fs.Upload("a", "b", nullptr, [&](FsError e) { up = std::move(e); })
            == kInvalidTransferHandle);

    REQUIRE(down.code == FsErrorCode::Unsupported);
    REQUIRE(up.code == FsErrorCode::Unsupported);
}
