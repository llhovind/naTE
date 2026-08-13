#include "fs/EditTempPath.h"

#include <filesystem>
#include <system_error>

namespace term::fs {

namespace {

// True when path is strictly inside the edit root. The root itself is excluded,
// which is what stops the walk-up from removing it, and so is anything that
// would have to traverse ".." to get there.
bool InsideEditRoot(const std::filesystem::path& path)
{
    const auto rel = path.lexically_relative(std::filesystem::path(kEditTempRoot));
    if (rel.empty()) return false;              // unrelated to the root
    if (rel.native() == ".") return false;      // is the root
    return rel.begin()->native() != "..";       // outside, reached by climbing
}

// Suffix mkdtemp(3) replaces with the unique component.
constexpr const char* kMkdtempSuffix = "XXXXXX";

// Flattens one path into a single directory name by escaping the separator.
// The remote directory becomes one local component so the tree stays shallow
// and a remote path can never climb out of kEditTempRoot: with no '/' left,
// there is nothing for ".." to traverse through.
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

} // namespace

EditTempPath MakeEditTempPath(const std::string& hostname,
                              const std::string& remotePath)
{
    std::string dir, file;
    SplitPath(remotePath, dir, file);

    // Every level is encoded, including the host: it is a description the
    // transport supplies, not something this layer gets to assume is a bare
    // hostname.
    std::string path = std::string(kEditTempRoot) + "/" + EncodeComponent(hostname);

    // A relative remote path has no directory level, and emitting an empty one
    // would put two separators in a row.
    if (!dir.empty())
        path += "/" + EncodeComponent(dir);

    path += "/";
    path += kMkdtempSuffix;

    return EditTempPath{std::move(path), std::move(file)};
}

void RemoveWorkingCopy(const std::string& localPath)
{
    const std::filesystem::path file(localPath);
    if (!InsideEditRoot(file))
        return;

    std::error_code ec;
    std::filesystem::remove(file, ec);

    // Climb while each level is one this removal just emptied. remove() reports
    // false for a directory that still has contents, which is exactly where the
    // walk should stop: another edit of the same remote file lives there.
    for (auto dir = file.parent_path(); InsideEditRoot(dir); dir = dir.parent_path()) {
        std::error_code dirEc;
        if (!std::filesystem::remove(dir, dirEc))
            break;
    }
}

} // namespace term::fs
