#include "fs/DirModel.h"

#include <algorithm>
#include <cctype>

namespace term::fs {

namespace {

char LowerByte(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Byte-wise case folding. Deliberately ASCII-only: remote names are opaque
// bytes in an unknown encoding, so anything more ambitious would be guessing.
// The effect is that ASCII names sort naturally and non-ASCII ones sort
// consistently, which is all a listing needs.
int CompareFold(const std::string& a, const std::string& b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(LowerByte(a[i]));
        const unsigned char cb = static_cast<unsigned char>(LowerByte(b[i]));
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

} // namespace

bool IsGlobPattern(const std::string& pattern) noexcept
{
    return pattern.find('*') != std::string::npos ||
           pattern.find('?') != std::string::npos;
}

bool GlobMatch(const std::string& pattern, const std::string& text)
{
    // Iterative backtracking rather than recursion: a pathological pattern
    // like "*a*a*a*..." against a long name would blow the stack otherwise,
    // and filenames come from outside naTE.
    size_t p = 0, t = 0;
    size_t starP = std::string::npos, starT = 0;

    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || LowerByte(pattern[p]) == LowerByte(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            // Remember the star and try matching it against nothing first.
            starP = p++;
            starT = t;
        } else if (starP != std::string::npos) {
            // Mismatch: let the last star swallow one more byte and retry.
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// ---------------------------------------------------------------------------

void DirModel::SetEntries(std::vector<transport::FileInfo> entries,
                          transport::FsError err)
{
    entries_ = std::move(entries);
    error_   = std::move(err);
    Rebuild();
}

void DirModel::Clear()
{
    entries_.clear();
    error_ = {};
    Rebuild();
}

void DirModel::SetShowHidden(bool show)
{
    if (showHidden_ == show) return;
    showHidden_ = show;
    Rebuild();
}

void DirModel::SetNameFilter(std::string pattern)
{
    if (nameFilter_ == pattern) return;
    nameFilter_ = std::move(pattern);
    Rebuild();
}

void DirModel::SetSort(SortKey key, SortOrder order)
{
    if (sortKey_ == key && sortOrder_ == order) return;
    sortKey_   = key;
    sortOrder_ = order;
    Rebuild();
}

void DirModel::SetDirectoriesFirst(bool first)
{
    if (directoriesFirst_ == first) return;
    directoriesFirst_ = first;
    Rebuild();
}

const transport::FileInfo& DirModel::At(size_t index) const
{
    return entries_[visible_[index]];
}

size_t DirModel::IndexOfName(const std::string& name) const
{
    for (size_t i = 0; i < visible_.size(); ++i)
        if (entries_[visible_[i]].name == name) return i;
    return visible_.size();
}

bool DirModel::PassesFilter(const transport::FileInfo& e) const
{
    if (!showHidden_ && !e.name.empty() && e.name.front() == '.')
        return false;

    if (nameFilter_.empty())
        return true;

    // Directories are exempt from the name filter. Filtering for "*.conf"
    // means "show me the config files here", not "hide the way to the rest of
    // the tree" — a filter that strands the user in a directory they cannot
    // navigate out of would be actively hostile.
    if (e.isDir)
        return true;

    if (IsGlobPattern(nameFilter_))
        return GlobMatch(nameFilter_, e.name);

    // Plain text: case-insensitive substring, which is what a user typing a
    // few characters into a filter box expects.
    if (nameFilter_.size() > e.name.size()) return false;
    const auto it = std::search(
        e.name.begin(), e.name.end(),
        nameFilter_.begin(), nameFilter_.end(),
        [](char a, char b) { return LowerByte(a) == LowerByte(b); });
    return it != e.name.end();
}

void DirModel::Rebuild()
{
    visible_.clear();
    visibleDirs_  = 0;
    visibleBytes_ = 0;

    visible_.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (!PassesFilter(entries_[i])) continue;
        visible_.push_back(i);
        if (entries_[i].isDir) ++visibleDirs_;
        else                   visibleBytes_ += entries_[i].size;
    }

    const bool descending = sortOrder_ == SortOrder::Descending;

    // Compares by the active key. Returns <0, 0 or >0 so the direction flip
    // and the dirs-first rule can be applied uniformly on top.
    const auto compareKey = [this](const transport::FileInfo& a,
                                   const transport::FileInfo& b) -> int {
        switch (sortKey_) {
            case SortKey::Size:
                if (a.size != b.size) return a.size < b.size ? -1 : 1;
                return 0;
            case SortKey::Modified:
                if (a.mtime != b.mtime) return a.mtime < b.mtime ? -1 : 1;
                return 0;
            case SortKey::Owner:
                if (const int c = CompareFold(a.owner, b.owner); c != 0) return c;
                return 0;
            case SortKey::Permissions:
                if (a.mode != b.mode) return a.mode < b.mode ? -1 : 1;
                return 0;
            case SortKey::Name:
                break;
        }
        return CompareFold(a.name, b.name);
    };

    std::stable_sort(visible_.begin(), visible_.end(),
        [&](size_t lhs, size_t rhs) {
            const transport::FileInfo& a = entries_[lhs];
            const transport::FileInfo& b = entries_[rhs];

            // Directories lead in both directions when enabled: reversing the
            // sort should reorder the files, not bury the way back up the tree.
            if (directoriesFirst_ && a.isDir != b.isDir)
                return a.isDir;

            int c = compareKey(a, b);
            // Every key except Name falls back to Name so that equal sizes or
            // timestamps produce a stable, meaningful order rather than an
            // arbitrary one.
            if (c == 0 && sortKey_ != SortKey::Name)
                c = CompareFold(a.name, b.name);
            if (c == 0) return false;
            return descending ? c > 0 : c < 0;
        });
}

} // namespace term::fs
