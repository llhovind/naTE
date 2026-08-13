#include "transport/LocalFileSystem.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace term::transport {

namespace {

// Maps errno onto the port's categories so callers can branch on the same
// codes whether the filesystem is local or remote.
FsErrorCode ClassifyErrno(int err)
{
    switch (err) {
        case ENOENT:
        case ENOTDIR:   return err == ENOTDIR ? FsErrorCode::NotADirectory
                                              : FsErrorCode::NoSuchFile;
        case EACCES:
        case EPERM:
        case EROFS:     return FsErrorCode::PermissionDenied;
        case EEXIST:    return FsErrorCode::AlreadyExists;
        case ENOTEMPTY: return FsErrorCode::DirectoryNotEmpty;
        default:        return FsErrorCode::LocalIoError;
    }
}

FsError ErrnoError(const std::string& context, int err)
{
    return FsError::Make(ClassifyErrno(err),
                         context + ": " + std::strerror(err));
}

// Resolves a uid to a login name, or empty when there is no such user.
// The explorer shows whichever it gets; a numeric fallback is the caller's.
std::string UserName(uid_t uid)
{
    if (const struct passwd* pw = ::getpwuid(uid)) return pw->pw_name;
    return {};
}

std::string GroupName(gid_t gid)
{
    if (const struct group* gr = ::getgrgid(gid)) return gr->gr_name;
    return {};
}

// Fills a FileInfo from an lstat result — link-not-target semantics, matching
// what SFTP's readdir reports, so both panes describe symlinks the same way.
void FillFromStat(const struct stat& st, FileInfo& info)
{
    info.size  = static_cast<uint64_t>(st.st_size);
    info.mode  = static_cast<uint32_t>(st.st_mode);
    info.uid   = static_cast<uint32_t>(st.st_uid);
    info.gid   = static_cast<uint32_t>(st.st_gid);
    info.mtime = static_cast<int64_t>(st.st_mtime);
    info.isDir     = S_ISDIR(st.st_mode);
    info.isSymlink = S_ISLNK(st.st_mode);
    info.owner = UserName(st.st_uid);
    info.group = GroupName(st.st_gid);
}

// Expands a leading "~" against $HOME. Only a leading one: "~" is shell
// syntax, and a tilde anywhere else in a path is an ordinary character.
std::string ExpandTilde(const std::string& path)
{
    if (path.empty() || path.front() != '~') return path;
    if (path.size() > 1 && path[1] != '/')   return path;   // ~user is not supported

    const char* home = std::getenv("HOME");
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

std::string JoinLocal(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

} // namespace

void LocalFileSystem::List(const std::string& path, ListCallback onDone)
{
    const std::string dir = ExpandTilde(path);

    DIR* handle = ::opendir(dir.c_str());
    if (!handle) {
        onDone({}, ErrnoError("Cannot list '" + dir + "'", errno));
        return;
    }

    std::vector<FileInfo> entries;
    FsError partial;

    while (const struct dirent* de = ::readdir(handle)) {
        const std::string name = de->d_name;
        if (name == "." || name == "..") continue;

        FileInfo info;
        info.name = name;

        struct stat st{};
        if (::lstat(JoinLocal(dir, name).c_str(), &st) == 0) {
            FillFromStat(st, info);
        } else if (partial.Ok()) {
            // A single unreadable entry should not lose the whole listing, so
            // it is reported alongside the rows that did read — the same
            // contract the SFTP adapter honours.
            partial = ErrnoError("Cannot stat '" + name + "'", errno);
        }
        entries.push_back(std::move(info));
    }
    ::closedir(handle);

    onDone(std::move(entries), std::move(partial));
}

void LocalFileSystem::RealPath(const std::string& path, PathCallback onDone)
{
    const std::string expanded = ExpandTilde(path);

    std::error_code ec;
    // weakly_canonical rather than canonical: a path whose tail does not exist
    // yet should still resolve its existing prefix instead of failing outright.
    const auto resolved = std::filesystem::weakly_canonical(expanded, ec);
    if (ec) {
        onDone({}, FsError::Make(ClassifyErrno(ec.value()),
                                 "Cannot resolve '" + expanded + "': " + ec.message()));
        return;
    }
    onDone(resolved.string(), FsError::Success());
}

void LocalFileSystem::Stat(const std::string& path, StatCallback onDone)
{
    const std::string target = ExpandTilde(path);

    struct stat st{};
    // stat, not lstat: the port defines Stat as following symlinks.
    if (::stat(target.c_str(), &st) != 0) {
        onDone({}, ErrnoError("Cannot stat '" + target + "'", errno));
        return;
    }

    FileInfo info;
    FillFromStat(st, info);
    onDone(std::move(info), FsError::Success());
}

void LocalFileSystem::ReadLink(const std::string& path, PathCallback onDone)
{
    const std::string target = ExpandTilde(path);

    char buf[PATH_MAX];
    const ssize_t n = ::readlink(target.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) {
        onDone({}, ErrnoError("Cannot read link '" + target + "'", errno));
        return;
    }
    onDone(std::string(buf, static_cast<size_t>(n)), FsError::Success());
}

void LocalFileSystem::MakeDirectory(const std::string& path, uint32_t mode,
                                    DoneCallback onDone)
{
    const std::string target = ExpandTilde(path);
    if (::mkdir(target.c_str(), static_cast<mode_t>(mode)) != 0) {
        onDone(ErrnoError("Cannot create directory '" + target + "'", errno));
        return;
    }
    onDone(FsError::Success());
}

void LocalFileSystem::Remove(const std::string& path, bool isDir,
                             DoneCallback onDone)
{
    const std::string target = ExpandTilde(path);
    // rmdir for directories, unlink for everything else — including symlinks
    // to directories, which must be removed as links.
    const int rc = isDir ? ::rmdir(target.c_str()) : ::unlink(target.c_str());
    if (rc != 0) {
        onDone(ErrnoError("Cannot delete '" + target + "'", errno));
        return;
    }
    onDone(FsError::Success());
}

void LocalFileSystem::CreateSymlink(const std::string& target,
                                    const std::string& linkPath,
                                    DoneCallback onDone)
{
    // target is not expanded: it is stored in the link verbatim, and a leading
    // "~" is a literal character to the kernel. Expanding it here would write a
    // different link than the one asked for.
    const std::string link = ExpandTilde(linkPath);
    if (::symlink(target.c_str(), link.c_str()) != 0) {
        onDone(ErrnoError("Cannot create link '" + link + "'", errno));
        return;
    }
    onDone(FsError::Success());
}

void LocalFileSystem::Rename(const std::string& from, const std::string& to,
                             DoneCallback onDone)
{
    const std::string src = ExpandTilde(from);
    const std::string dst = ExpandTilde(to);

    // POSIX rename() silently replaces an existing destination, so refuse one
    // outright. Callers that want to overwrite must remove the target first
    // and mean it.
    struct stat st{};
    if (::lstat(dst.c_str(), &st) == 0) {
        onDone(FsError::Make(FsErrorCode::AlreadyExists,
                             "Cannot rename to '" + dst + "': already exists"));
        return;
    }

    if (::rename(src.c_str(), dst.c_str()) != 0) {
        onDone(ErrnoError("Cannot rename '" + src + "'", errno));
        return;
    }
    onDone(FsError::Success());
}

void LocalFileSystem::SetPermissions(const std::string& path, uint32_t mode,
                                     DoneCallback onDone)
{
    const std::string target = ExpandTilde(path);
    if (::chmod(target.c_str(), static_cast<mode_t>(mode)) != 0) {
        onDone(ErrnoError("Cannot set permissions on '" + target + "'", errno));
        return;
    }
    onDone(FsError::Success());
}

// ---------------------------------------------------------------------------
// Transfers — a copy from one path on this disk to another
// ---------------------------------------------------------------------------

namespace {

// I/O chunk size for a copy. Large enough that the syscall overhead disappears
// against the transfer, small enough that a cancellation is noticed promptly.
constexpr size_t kCopyChunk = 65536;

// Minimum gap between progress callbacks, matching the SFTP transfers': a local
// copy can move hundreds of chunks a second, and every callback wakes the UI.
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

// Owns a descriptor for the duration of one copy. close() is where a deferred
// write error surfaces on some filesystems, so the destination is closed
// deliberately through Close() and the destructor is only the safety net.
class Fd {
public:
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) ::close(fd_); }

    Fd(const Fd&)            = delete;
    Fd& operator=(const Fd&) = delete;

    int  Get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    // Closes and reports whether the close itself succeeded.
    bool Close() noexcept
    {
        if (fd_ < 0) return true;
        const int rc = ::close(fd_);
        fd_ = -1;
        return rc == 0;
    }

private:
    int fd_;
};

FsError Cancelled()
{
    return FsError::Make(FsErrorCode::Cancelled, "Copy cancelled");
}

} // namespace

LocalFileSystem::~LocalFileSystem()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopping_ = true;
        // A copy already under way is told to stop as well, or destruction
        // would wait out however many gigabytes are left.
        for (auto& [handle, flag] : live_)
            flag->store(true, std::memory_order_release);
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// Both verbs are one operation: copy the first path to the second. Which
// argument the port calls "local" is meaningless here, where both are.
TransferHandle LocalFileSystem::Download(const std::string& remotePath,
                                         const std::string& localPath,
                                         std::optional<uint32_t> sourceMode,
                                         ProgressCallback onProgress,
                                         DoneCallback onDone)
{
    return StartCopy(remotePath, localPath, sourceMode,
                     std::move(onProgress), std::move(onDone));
}

TransferHandle LocalFileSystem::Upload(const std::string& localPath,
                                       const std::string& remotePath,
                                       std::optional<uint32_t> sourceMode,
                                       ProgressCallback onProgress,
                                       DoneCallback onDone)
{
    return StartCopy(localPath, remotePath, sourceMode,
                     std::move(onProgress), std::move(onDone));
}

TransferHandle LocalFileSystem::StartCopy(const std::string& from,
                                          const std::string& to,
                                          std::optional<uint32_t> mode,
                                          ProgressCallback onProgress,
                                          DoneCallback onDone)
{
    CopyJob job;
    job.from       = from;
    job.to         = to;
    job.mode       = mode;
    job.onProgress = std::move(onProgress);
    job.onDone     = std::move(onDone);
    job.cancelled  = std::make_shared<std::atomic<bool>>(false);

    TransferHandle handle = kInvalidTransferHandle;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!stopping_) {
            handle     = nextHandle_++;
            job.handle = handle;
            live_.emplace(handle, job.cancelled);
            // The worker starts on the first copy, so an explorer that never
            // moves a local file never pays for a thread.
            if (!worker_.joinable())
                worker_ = std::thread([this] { WorkerLoop(); });
            queue_.push_back(std::move(job));
        }
    }

    // Refused: only reachable from a caller racing destruction, which is a bug
    // on its side. The callback still fires exactly once, and outside the lock
    // — running caller code under it is how a callback that comes back in here
    // would deadlock.
    if (handle == kInvalidTransferHandle) {
        if (job.onDone) job.onDone(Cancelled());
        return kInvalidTransferHandle;
    }

    wake_.notify_one();
    // Reported from the local, not from the queue: by now the worker may have
    // popped the job and finished it.
    return handle;
}

void LocalFileSystem::WorkerLoop()
{
    for (;;) {
        CopyJob job;
        bool    abandon = false;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            wake_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
            // Stopping does not discard what is queued: every callback is owed
            // exactly one answer, so the rest are retired as cancelled rather
            // than dropped.
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            abandon = stopping_;
        }

        Finish(job, abandon ? Cancelled() : RunCopy(job));
    }
}

FsError LocalFileSystem::RunCopy(const CopyJob& job)
{
    const auto cancelled = [&job] {
        return job.cancelled->load(std::memory_order_acquire);
    };
    if (cancelled()) return Cancelled();

    const std::string from = ExpandTilde(job.from);
    const std::string to   = ExpandTilde(job.to);

    Fd src(::open(from.c_str(), O_RDONLY | O_CLOEXEC));
    if (!src) return ErrnoError("Cannot read '" + from + "'", errno);

    struct stat srcStat{};
    if (::fstat(src.Get(), &srcStat) != 0)
        return ErrnoError("Cannot stat '" + from + "'", errno);
    // A directory opens for reading on Linux and only fails at the first read,
    // which would report the wrong thing at the wrong moment. Recursion is a
    // higher layer's job; this verb moves one file.
    if (S_ISDIR(srcStat.st_mode))
        return ErrnoError("Cannot copy '" + from + "'", EISDIR);

    // Refuse to copy a file onto itself. The truncating open below would
    // destroy the source before a byte had been read, so this is data loss
    // rather than a wasted call — cp(1) refuses it for the same reason.
    // Comparing device and inode catches what a string compare misses: a hard
    // link, a symlink, and two spellings of one path.
    struct stat dstStat{};
    if (::stat(to.c_str(), &dstStat) == 0 &&
        dstStat.st_dev == srcStat.st_dev && dstStat.st_ino == srcStat.st_ino) {
        return FsError::Make(FsErrorCode::AlreadyExists,
                             "'" + from + "' and '" + to + "' are the same file");
    }

    const auto bits = static_cast<mode_t>(
        job.mode.value_or(kDefaultFileMode) & kModeBitsMask);
    Fd dst(::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, bits));
    if (!dst) return ErrnoError("Cannot write '" + to + "'", errno);

    const uint64_t total = static_cast<uint64_t>(srcStat.st_size);
    uint64_t       copied = 0;
    // Dated so the first chunk reports immediately: a copy that takes a moment
    // to get going should still show that it started.
    auto lastProgress = std::chrono::steady_clock::now() - kProgressInterval;

    std::vector<char> buf(kCopyChunk);
    for (;;) {
        if (cancelled()) return Cancelled();

        const ssize_t n = ::read(src.Get(), buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return ErrnoError("Cannot read '" + from + "'", errno);
        }
        if (n == 0) break;

        size_t written = 0;
        while (written < static_cast<size_t>(n)) {
            const ssize_t w =
                ::write(dst.Get(), buf.data() + written,
                        static_cast<size_t>(n) - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                return ErrnoError("Cannot write '" + to + "'", errno);
            }
            written += static_cast<size_t>(w);
        }
        copied += static_cast<uint64_t>(n);

        const auto now = std::chrono::steady_clock::now();
        if (job.onProgress && now - lastProgress >= kProgressInterval) {
            lastProgress = now;
            job.onProgress(copied, total);
        }
    }

    if (!dst.Close())
        return ErrnoError("Cannot finish writing '" + to + "'", errno);

    if (job.onProgress) job.onProgress(copied, total);
    return FsError::Success();
}

void LocalFileSystem::Finish(const CopyJob& job, FsError err)
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        live_.erase(job.handle);
    }
    // Outside the lock: the callback runs arbitrary caller code, which may
    // start the next copy.
    if (job.onDone) job.onDone(std::move(err));
}

void LocalFileSystem::Cancel(TransferHandle handle)
{
    std::shared_ptr<std::atomic<bool>> flag;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = live_.find(handle);
        // An unknown or already-finished handle is not an error; the port says
        // so, and a caller cancelling a race it lost must not be punished.
        if (it == live_.end()) return;
        flag = it->second;
    }
    flag->store(true, std::memory_order_release);
}

} // namespace term::transport
