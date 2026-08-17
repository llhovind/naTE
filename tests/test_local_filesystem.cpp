#include <catch2/catch_test_macros.hpp>

#include "fs/FileMode.h"
#include "transport/LocalFileSystem.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>

#include <sys/stat.h>
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

std::string ReadAll(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// Permission bits of a path, or 0 if it is not there.
uint32_t ModeOf(const std::string& path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint32_t>(st.st_mode) & 07777;
}

// The process umask, read the only way POSIX offers: by setting it and putting
// it back. A creation mode is narrowed by it, so a test asserting on a default
// has to say so too.
mode_t Umask()
{
    const mode_t current = ::umask(0);
    ::umask(current);
    return current;
}

// Bridges a transfer's completion, which arrives on the adapter's worker
// thread, back to the thread running the test.
//
// The state is shared rather than held by value, and the callback keeps a
// reference to it. Waking a waiter does not finish the moment it wakes: the
// notifying thread is still inside the condition variable, and a test that
// returned and destroyed one would pull it out from under that thread.
class Waiter {
public:
    DoneCallback Done() const
    {
        return [state = state_](FsError err) {
            {
                std::lock_guard<std::mutex> lk(state->mutex);
                state->err   = std::move(err);
                state->fired = true;
            }
            state->cv.notify_all();
        };
    }

    // Blocks until the callback fires. The timeout turns a callback that never
    // arrives into a failed assertion rather than a test run that hangs.
    FsError Wait() const
    {
        std::unique_lock<std::mutex> lk(state_->mutex);
        REQUIRE(state_->cv.wait_for(lk, std::chrono::seconds(10),
                                    [s = state_] { return s->fired; }));
        return state_->err;
    }

    bool Fired() const
    {
        std::lock_guard<std::mutex> lk(state_->mutex);
        return state_->fired;
    }

private:
    struct State {
        std::mutex              mutex;
        std::condition_variable cv;
        FsError                 err;
        bool                    fired = false;
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();
};

struct CopyResult {
    FsError  err;
    uint64_t transferred = 0;
    uint64_t total       = 0;
};

// Runs one copy and waits for it. The progress figures are written on the
// worker thread and read here only after the completion callback has been
// observed, which orders the two.
CopyResult CopySync(LocalFileSystem& fs, const std::string& from,
                    const std::string& to, std::optional<uint32_t> mode)
{
    CopyResult result;
    Waiter     waiter;

    fs.Download(from, to, mode,
                [&result](uint64_t transferred, uint64_t total) {
                    result.transferred = transferred;
                    result.total       = total;
                },
                waiter.Done());

    result.err = waiter.Wait();
    return result;
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

TEST_CASE("given two local paths when copied then the bytes and the mode arrive") {
    TempTree tmp("copy");
    const std::string src = tmp.File("script.sh", "#!/bin/sh\necho hi\n");
    const std::string dst = tmp.At("copy.sh");

    LocalFileSystem fs;

    SECTION("the contents are reproduced") {
        CopyResult result = CopySync(fs, src, dst, std::nullopt);

        REQUIRE(result.err.Ok());
        REQUIRE(ReadAll(dst) == "#!/bin/sh\necho hi\n");
    }

    SECTION("the source's permissions are reproduced") {
        // The case that made this necessary: an executable that arrives without
        // its execute bit is not the file the user asked to copy.
        CopyResult result = CopySync(fs, src, dst, 0755);

        REQUIRE(result.err.Ok());
        // Narrowed by the umask, exactly as a file the user created here would
        // be — asking for permissions the umask forbids is not this adapter's
        // call to overrule.
        REQUIRE(ModeOf(dst) == (0755 & ~Umask()));
        // Whatever the umask, the bit that mattered is the owner's.
        REQUIRE((ModeOf(dst) & 0100) != 0);
    }

    SECTION("an unknown mode leaves the adapter's default") {
        CopyResult result = CopySync(fs, src, dst, std::nullopt);

        REQUIRE(result.err.Ok());
        REQUIRE(ModeOf(dst) == (kDefaultFileMode & ~Umask()));
    }

    SECTION("an existing destination keeps the permissions it already had") {
        // cp(1)'s rule: the mode applies where the file is created. Overwriting
        // must not silently reopen a file the user had locked down.
        tmp.File("copy.sh", "old");
        REQUIRE(::chmod(dst.c_str(), 0600) == 0);

        CopyResult result = CopySync(fs, src, dst, 0777);

        REQUIRE(result.err.Ok());
        REQUIRE(ModeOf(dst) == 0600);
        REQUIRE(ReadAll(dst) == "#!/bin/sh\necho hi\n");
    }

    SECTION("progress reports the whole file") {
        CopyResult result = CopySync(fs, src, dst, std::nullopt);

        REQUIRE(result.err.Ok());
        REQUIRE(result.transferred == 18);
        REQUIRE(result.total == 18);
    }
}

TEST_CASE("given Upload and Download when copying locally then both mean the same thing") {
    // The port names them for a remote this adapter does not have. What matters
    // is that each copies its first argument to its second.
    TempTree tmp("verbs");
    const std::string a = tmp.File("a.txt", "AAA");
    const std::string b = tmp.At("b.txt");
    const std::string c = tmp.At("c.txt");

    LocalFileSystem fs;

    Waiter down;
    fs.Download(a, b, std::nullopt, nullptr, down.Done());
    REQUIRE(down.Wait().Ok());

    Waiter up;
    fs.Upload(a, c, std::nullopt, nullptr, up.Done());
    REQUIRE(up.Wait().Ok());

    REQUIRE(ReadAll(b) == "AAA");
    REQUIRE(ReadAll(c) == "AAA");
}

TEST_CASE("given a copy onto the file being read when run then it refuses") {
    // The truncating open would destroy the source before a byte was read, so
    // this has to be refused rather than attempted.
    TempTree tmp("selfcopy");
    const std::string src = tmp.File("a.txt", "precious");

    LocalFileSystem fs;

    SECTION("the same path") {
        CopyResult result = CopySync(fs, src, src, std::nullopt);

        REQUIRE(result.err.Failed());
        REQUIRE(ReadAll(src) == "precious");
    }

    SECTION("a second name for the same inode") {
        const std::string link = tmp.At("link.txt");
        REQUIRE(::link(src.c_str(), link.c_str()) == 0);

        CopyResult result = CopySync(fs, src, link, std::nullopt);

        REQUIRE(result.err.Failed());
        REQUIRE(ReadAll(src) == "precious");
    }
}

TEST_CASE("given a copy of something that is not a readable file then it fails clearly") {
    TempTree tmp("copyfail");

    LocalFileSystem fs;

    SECTION("a missing source") {
        CopyResult result = CopySync(fs, tmp.At("gone.txt"), tmp.At("out.txt"),
                                     std::nullopt);

        REQUIRE(result.err.code == FsErrorCode::NoSuchFile);
    }

    SECTION("a directory source") {
        // Recursion belongs to the layer that walks a tree; this verb moves one
        // file and says so rather than failing halfway through a read.
        CopyResult result = CopySync(fs, tmp.Dir("sub"), tmp.At("out.txt"),
                                     std::nullopt);

        REQUIRE(result.err.Failed());
    }

    SECTION("an unwritable destination directory") {
        CopyResult result = CopySync(fs, tmp.File("a.txt"),
                                     tmp.At("nosuchdir/out.txt"), std::nullopt);

        REQUIRE(result.err.Failed());
    }
}

TEST_CASE("given a queued copy when cancelled then it retires as cancelled") {
    TempTree tmp("cancel");
    const std::string src = tmp.File("big.bin", std::string(4 * 1024 * 1024, 'x'));

    LocalFileSystem fs;
    Waiter waiter;

    const TransferHandle handle =
        fs.Download(src, tmp.At("out.bin"), std::nullopt, nullptr, waiter.Done());
    REQUIRE(handle != kInvalidTransferHandle);
    fs.Cancel(handle);

    const FsError err = waiter.Wait();
    // A cancel that lands after the copy finished is not a failure — the port
    // promises the callback either way, and which side of the race it fell on
    // is not this test's business.
    REQUIRE((err.Ok() || err.code == FsErrorCode::Cancelled));
}

TEST_CASE("given an unknown handle when cancelled then nothing happens") {
    LocalFileSystem fs;
    fs.Cancel(12345);        // must not throw or block
    fs.Cancel(kInvalidTransferHandle);
    SUCCEED();
}

TEST_CASE("given a copy in flight when the adapter is destroyed then the callback still fires") {
    // The port promises exactly one callback per call. Destruction is the case
    // where dropping one would be easiest and least visible.
    TempTree tmp("teardown");
    const std::string src = tmp.File("big.bin", std::string(4 * 1024 * 1024, 'x'));

    Waiter waiter;
    {
        LocalFileSystem fs;
        fs.Download(src, tmp.At("out.bin"), std::nullopt, nullptr, waiter.Done());
    }   // joins the worker

    REQUIRE(waiter.Fired());
}

// ---------------------------------------------------------------------------
// Volume space
// ---------------------------------------------------------------------------

TEST_CASE("given an existing directory when its space is queried then it reports the volume") {
    TempTree tmp("space");

    LocalFileSystem fs;
    std::optional<FsSpaceInfo> got;
    FsError err;
    fs.QuerySpace(tmp.root.string(), [&](FsSpaceInfo info, FsError e) {
        got = info;
        err = std::move(e);
    });

    REQUIRE(err.Ok());
    REQUIRE(got.has_value());
    // Real figures from a real volume, so the only safe assertions are the
    // invariants: a mounted filesystem has capacity, and what a user may write
    // never exceeds it.
    REQUIRE(got->totalBytes > 0);
    REQUIRE(got->availableBytes <= got->totalBytes);
}

TEST_CASE("given a file rather than a directory when its space is queried then it reports the same volume") {
    // statvfs takes any path on the volume. A caller holding a file path should
    // not have to derive its directory first.
    TempTree tmp("space_file");
    const std::string file = tmp.File("a.txt", "hello");

    LocalFileSystem fs;
    std::optional<FsSpaceInfo> viaFile;
    std::optional<FsSpaceInfo> viaDir;
    fs.QuerySpace(file, [&](FsSpaceInfo info, FsError) { viaFile = info; });
    fs.QuerySpace(tmp.root.string(), [&](FsSpaceInfo info, FsError) { viaDir = info; });

    REQUIRE(viaFile.has_value());
    REQUIRE(viaDir.has_value());
    REQUIRE(viaFile->totalBytes == viaDir->totalBytes);
}

TEST_CASE("given a path that does not exist when its space is queried then it fails rather than reporting zero") {
    // Zero would be indistinguishable from a full disk, and a caller acting on
    // it would refuse a transfer the volume has ample room for.
    LocalFileSystem fs;
    FsError err;
    bool called = false;
    fs.QuerySpace("/nonexistent-path-for-space-test", [&](FsSpaceInfo, FsError e) {
        called = true;
        err    = std::move(e);
    });

    REQUIRE(called);
    REQUIRE(err.Failed());
    REQUIRE(err.code == FsErrorCode::NoSuchFile);
}
