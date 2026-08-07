#pragma once
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <string>
#include <vector>

namespace term::fs {

// Which column a listing is ordered by.
enum class SortKey { Name, Size, Modified, Owner, Permissions };

enum class SortOrder { Ascending, Descending };

// The contents of one directory, projected for display.
//
// A pure store plus a projection: it holds what a listing returned and answers
// "what should row N show", applying the current filter and sort. It performs
// no I/O and knows nothing about wx — the controller fetches, this decides what
// is visible and in what order.
//
// Selection is deliberately *not* held here. A virtual list control owns its
// own selection state, and mirroring it would create two sources of truth that
// drift the moment a sort or filter reorders the rows.
class DirModel {
public:
    // Replaces the contents. err carries a partial-failure outcome: a listing
    // that failed halfway still delivers the entries it read, and both are
    // kept so the view can show the rows *and* say the result is incomplete.
    void SetEntries(std::vector<transport::FileInfo> entries,
                    transport::FsError err = {});

    // Discards entries and error, e.g. when navigating away.
    void Clear();

    const std::string& Path() const noexcept { return path_; }
    void SetPath(std::string path) { path_ = std::move(path); }

    // --- Outcome of the listing that produced these entries -----------------
    bool HasError() const noexcept { return error_.Failed(); }
    const transport::FsError& Error() const noexcept { return error_; }
    // True when the listing failed but still returned rows.
    bool IsPartial() const noexcept { return error_.Failed() && !entries_.empty(); }

    // --- Filtering -----------------------------------------------------------
    // Dotfiles are hidden by default, as every POSIX file manager does.
    void SetShowHidden(bool show);
    bool ShowHidden() const noexcept { return showHidden_; }

    // A name filter. Patterns containing '*' or '?' are matched as globs
    // ("*.conf"); anything else is a case-insensitive substring match, which
    // is what a user typing into a filter box almost always means. An empty
    // filter matches everything.
    void SetNameFilter(std::string pattern);
    const std::string& NameFilter() const noexcept { return nameFilter_; }

    // --- Sorting -------------------------------------------------------------
    void SetSort(SortKey key, SortOrder order);
    SortKey   Sort()      const noexcept { return sortKey_; }
    SortOrder Order()     const noexcept { return sortOrder_; }

    // When true (the default) directories precede files regardless of the sort
    // key or direction, which is what makes a listing navigable.
    void SetDirectoriesFirst(bool first);
    bool DirectoriesFirst() const noexcept { return directoriesFirst_; }

    // --- Projection ----------------------------------------------------------
    // Number of rows after filtering.
    size_t VisibleCount() const noexcept { return visible_.size(); }
    // Number of entries before filtering.
    size_t TotalCount() const noexcept { return entries_.size(); }

    // Row accessor. Callers must keep index < VisibleCount(); the reference is
    // invalidated by any mutating call.
    const transport::FileInfo& At(size_t index) const;

    // Counts and byte total over the *visible* rows — what a status line
    // reports, and what changes when the filter changes.
    size_t   VisibleDirectoryCount() const noexcept { return visibleDirs_; }
    size_t   VisibleFileCount() const noexcept { return visible_.size() - visibleDirs_; }
    uint64_t VisibleByteTotal() const noexcept { return visibleBytes_; }

    // Finds the row currently showing name, or VisibleCount() if the filter
    // hides it or it is absent. Lets a caller restore the cursor onto the same
    // file after a refresh reorders the listing.
    size_t IndexOfName(const std::string& name) const;

private:
    // Recomputes visible_ from entries_ using the current filter and sort.
    void Rebuild();
    bool PassesFilter(const transport::FileInfo& e) const;

    std::string                      path_;
    std::vector<transport::FileInfo> entries_;
    transport::FsError               error_;

    // Indices into entries_, in display order. Indices rather than copies so a
    // re-sort never duplicates the entry data.
    std::vector<size_t> visible_;
    size_t   visibleDirs_  = 0;
    uint64_t visibleBytes_ = 0;

    bool        showHidden_       = false;
    std::string nameFilter_;
    SortKey     sortKey_          = SortKey::Name;
    SortOrder   sortOrder_        = SortOrder::Ascending;
    bool        directoriesFirst_ = true;
};

// --- Pattern matching, exposed for testing ---------------------------------

// Case-insensitive glob over bytes. Supports '*' (any run, including empty)
// and '?' (exactly one byte). Everything else matches literally.
bool GlobMatch(const std::string& pattern, const std::string& text);

// True when pattern contains a glob metacharacter.
bool IsGlobPattern(const std::string& pattern) noexcept;

} // namespace term::fs
