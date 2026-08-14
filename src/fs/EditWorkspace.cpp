#include "fs/EditWorkspace.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace term::fs {

namespace {

// Suffix mkdtemp(3) replaces with the unique component, and the separator that
// divides it from the owner pid in front.
constexpr const char* kMkdtempSuffix = "XXXXXX";
constexpr char        kOwnerSeparator = '-';

// Flattens one path into a single directory name by escaping the separator.
// The remote directory becomes one local component so the tree stays shallow
// and a remote path can never climb out of the root: with no '/' left, there is
// nothing for ".." to traverse through.
std::string EncodeComponent(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '/') out += "%2F";
        else          out += c;
    }
    return out;
}

// Splits a remote path into its directory and basename, ignoring trailing
// separators so a path written with one still yields the name before it.
void SplitPath(const std::string& path, std::string& dir, std::string& file)
{
    size_t end = path.size();
    while (end > 0 && path[end - 1] == '/') --end;

    const std::string trimmed = path.substr(0, end);
    const auto slash = trimmed.rfind('/');
    if (slash == std::string::npos) {
        dir.clear();
        file = trimmed;
        return;
    }
    dir  = trimmed.substr(0, slash);
    file = trimmed.substr(slash + 1);
}

// True when path is strictly inside the root. The root itself is excluded,
// which is what stops the walk-up from removing it, and so is anything that
// would have to traverse ".." to get there.
bool InsideRoot(const std::filesystem::path& path)
{
    const auto rel = path.lexically_relative(std::filesystem::path(kEditWorkspaceRoot));
    if (rel.empty()) return false;              // unrelated to the root
    if (rel.native() == ".") return false;      // is the root
    return rel.begin()->native() != "..";       // outside, reached by climbing
}

// Removes each directory above start that the caller's removal just emptied,
// stopping at the first that is still in use. remove() reports false for a
// non-empty directory, which is exactly where the walk should stop: another
// edit of the same remote file lives there.
void PruneEmptyAncestors(std::filesystem::path start)
{
    for (auto dir = std::move(start); InsideRoot(dir); dir = dir.parent_path()) {
        std::error_code ec;
        if (!std::filesystem::remove(dir, ec))
            break;
    }
}

// Reads a process's name from /proc, or nullopt when there is no such process.
std::optional<std::string> ProcessName(int pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    if (!in) return std::nullopt;

    std::string name;
    if (!std::getline(in, name)) return std::nullopt;
    return name;
}

} // namespace

WorkingCopyPath MakeWorkingCopyPath(const std::string& hostname,
                                    const std::string& remotePath,
                                    int ownerPid)
{
    std::string dir, file;
    SplitPath(remotePath, dir, file);

    // Every level is encoded, including the host: it is a description the
    // transport supplies, not something this layer gets to assume is a bare
    // hostname.
    std::string path = std::string(kEditWorkspaceRoot) + "/" + EncodeComponent(hostname);

    // A relative remote path has no directory level, and emitting an empty one
    // would put two separators in a row.
    if (!dir.empty())
        path += "/" + EncodeComponent(dir);

    path += "/" + std::to_string(ownerPid) + kOwnerSeparator + kMkdtempSuffix;

    return WorkingCopyPath{std::move(path), std::move(file)};
}

std::optional<int> OwnerPidOfWorkingCopyDir(const std::string& dirName)
{
    const auto sep = dirName.find(kOwnerSeparator);
    if (sep == std::string::npos || sep == 0)
        return std::nullopt;

    const std::string digits = dirName.substr(0, sep);
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
    }

    // A pid long enough to overflow was never written by this scheme, so the
    // name is not one of ours whatever else it is.
    try {
        return std::stoi(digits);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void RemoveWorkingCopy(const std::string& localPath)
{
    const std::filesystem::path file(localPath);
    if (!InsideRoot(file))
        return;

    std::error_code ec;
    std::filesystem::remove(file, ec);
    PruneEmptyAncestors(file.parent_path());
}

size_t PurgeOrphanedWorkingCopies(const OwnerIsLiveFn& isLive)
{
    const std::filesystem::path root(kEditWorkspaceRoot);

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return 0;

    // Collected first rather than removed during the walk: deleting entries
    // out from under a recursive iterator is undefined, and the prune afterwards
    // deletes ancestors the iterator would still be standing in.
    std::vector<std::filesystem::path> orphans;

    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return 0;

    for (const auto& entry : it) {
        std::error_code dirEc;
        if (!entry.is_directory(dirEc)) continue;

        const auto owner = OwnerPidOfWorkingCopyDir(entry.path().filename().string());
        if (!owner) continue;          // not a working copy: not ours to judge
        if (isLive(*owner)) continue;  // another instance is editing in there

        orphans.push_back(entry.path());
    }

    size_t reclaimed = 0;
    for (const auto& dir : orphans) {
        std::error_code rmEc;
        std::filesystem::remove_all(dir, rmEc);
        if (rmEc) continue;
        ++reclaimed;
        PruneEmptyAncestors(dir.parent_path());
    }
    return reclaimed;
}

bool DefaultOwnerIsLive(int pid)
{
    if (pid <= 0) return false;

    // Nothing of this run can have written a directory yet, so a directory
    // wearing our pid is a dead instance whose number we inherited.
    if (pid == ::getpid()) return false;

    const auto owner = ProcessName(pid);
    if (!owner) return false;

    const auto self = ProcessName(::getpid());
    return self && *owner == *self;
}

} // namespace term::fs
