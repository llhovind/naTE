#include "fs/RemotePath.h"

#include <vector>

namespace term::fs::path {

namespace {

// Strips trailing separators, but never reduces "/" to "".
std::string TrimTrailingSlashes(const std::string& path)
{
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    return path.substr(0, end);
}

} // namespace

bool IsAbsolute(const std::string& path) noexcept
{
    return !path.empty() && path.front() == '/';
}

std::string Join(const std::string& dir, const std::string& leaf)
{
    if (IsAbsolute(leaf)) return leaf;
    if (leaf.empty())     return dir;
    if (dir.empty())      return leaf;
    if (dir.back() == '/') return dir + leaf;
    return dir + "/" + leaf;
}

std::string Leaf(const std::string& path)
{
    if (path.empty()) return {};

    const std::string trimmed = TrimTrailingSlashes(path);
    if (trimmed == "/") return "/";

    const auto pos = trimmed.rfind('/');
    return (pos == std::string::npos) ? trimmed : trimmed.substr(pos + 1);
}

std::string Parent(const std::string& path)
{
    if (path.empty()) return {};

    const std::string trimmed = TrimTrailingSlashes(path);
    if (trimmed == "/") return "/";

    const auto pos = trimmed.rfind('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0)                 return "/";
    return trimmed.substr(0, pos);
}

std::string Normalise(const std::string& path)
{
    if (path.empty()) return {};

    const bool absolute = IsAbsolute(path);

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        const size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i == start) break;

        const std::string comp = path.substr(start, i - start);
        if (comp == ".") continue;

        if (comp == "..") {
            // Pop a real component if there is one. Otherwise keep the ".."
            // on a relative path (no base to resolve against), or drop it on
            // an absolute path, where the root is its own parent.
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(comp);
            }
            continue;
        }
        parts.push_back(comp);
    }

    std::string out;
    for (const auto& p : parts) {
        if (!out.empty() || absolute) out += '/';
        out += p;
    }

    if (out.empty()) return absolute ? "/" : ".";
    return out;
}

bool Contains(const std::string& dir, const std::string& path)
{
    const std::string base   = Normalise(dir);
    const std::string target = Normalise(path);

    if (base == target) return true;
    // The separator has to be part of the comparison, or "/etc" would be found
    // to contain "/etcetera".
    const std::string prefix = base == "/" ? base : base + "/";
    return target.compare(0, prefix.size(), prefix) == 0;
}

} // namespace term::fs::path
