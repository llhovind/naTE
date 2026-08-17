#include "fs/EditWorkspace.h"

#include <filesystem>
#include <system_error>
#include <vector>

namespace term::fs {

namespace {

// Suffix mkdtemp(3) replaces with the unique component. What divides it from
// the owner pid in front is kOwnerSeparator, which this scheme shares with the
// staging area rather than declaring its own copy of.
constexpr const char* kMkdtempSuffix = "XXXXXX";

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

        const auto owner = OwnerPidOfTaggedName(entry.path().filename().string());
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

} // namespace term::fs
