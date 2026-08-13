#include "transport/LocalFileSystem.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <dirent.h>
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
// Transfers — not this adapter's job
// ---------------------------------------------------------------------------

namespace {

FsError NotATransferEndpoint()
{
    return FsError::Make(FsErrorCode::Unsupported,
                         "The local filesystem is not a transfer endpoint");
}

} // namespace

TransferHandle LocalFileSystem::Download(const std::string&, const std::string&,
                                         ProgressCallback, DoneCallback onDone)
{
    if (onDone) onDone(NotATransferEndpoint());
    return kInvalidTransferHandle;
}

TransferHandle LocalFileSystem::Upload(const std::string&, const std::string&,
                                       ProgressCallback, DoneCallback onDone)
{
    if (onDone) onDone(NotATransferEndpoint());
    return kInvalidTransferHandle;
}

void LocalFileSystem::Cancel(TransferHandle) {}

} // namespace term::transport
