#pragma once
#include "fs/LinkTarget.h"
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <string>
#include <vector>

namespace term::fs {

// Which column a listing is ordered by.
enum class SortKey { Name, Size, Modified, Owner, Permissions };

enum class SortOrder { Ascending, Descending };

// What a freshly opened listing starts from.
//
// Compile-time constants rather than AppConfig fields, and deliberately so: a
// remembered sort is as likely to confuse as to help, because a window that
// opens ordered by Permissions on account of something the user did last week
// reads as a bug rather than as a preference being honoured. Hidden files
// follow `ls`, which shows dotfiles only when asked — the convention every
// administrator already has in their fingers.
//
// Promoting either to a real preference is mechanical, which is the point of
// naming them here: add the field to AppConfig, thread it to the two places
// these are read (DirModel's own initial state, and the toolbar checkbox in
// FileExplorerPane::BuildToolbar), and add the control to PreferencesDialog.
inline constexpr bool       kDefaultShowHidden = false;
inline constexpr SortKey    kDefaultSortKey    = SortKey::Name;
inline constexpr SortOrder  kDefaultSortOrder  = SortOrder::Ascending;

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
    //
    // Every link comes back Unresolved: these entries describe links, not what
    // they point at, and a previous listing's answers cannot be carried over
    // because the thing at the other end is exactly what may have changed.
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

    // A name filter. Patterns containing '*' or '?' are matched as globs
    // ("*.conf"); anything else is a case-insensitive substring match, which
    // is what a user typing into a filter box almost always means. An empty
    // filter matches everything.
    void SetNameFilter(std::string pattern);

    // --- Symbolic links ------------------------------------------------------
    // Leaf names of every link whose target is still unknown, whether or not
    // the filter is currently showing it. The model is asked rather than told
    // because it is the one that knows which entries are links and which have
    // already been answered for; a caller that tracked that itself would have a
    // second copy to keep in step.
    std::vector<std::string> UnresolvedLinkNames() const;

    // Records what links point at, re-sorting once for the whole batch. Names
    // that are no longer in the listing are ignored: an answer that arrives
    // after the directory was replaced describes rows that have gone.
    void ApplyLinkTargets(const std::vector<LinkResolution>& resolutions);

    // What the row's link points at. Unresolved for anything that is not a
    // link, which is the honest answer to "where does this lead" for a file.
    LinkTarget LinkTargetAt(size_t index) const;

    // True when the row behaves as a directory: a real one, or a link known to
    // lead to one. This is what "directories first" sorts on and what a view
    // should ask before it decorates a row as enterable — a link to a directory
    // is a doorway, and burying it among the files hides the way through.
    bool IsDirectoryLike(size_t index) const;

    // --- Sorting -------------------------------------------------------------
    void SetSort(SortKey key, SortOrder order);
    SortKey   Sort()      const noexcept { return sortKey_; }
    SortOrder Order()     const noexcept { return sortOrder_; }

    // When true (the default) directories precede files regardless of the sort
    // key or direction, which is what makes a listing navigable.
    void SetDirectoriesFirst(bool first);

    // --- Projection ----------------------------------------------------------
    // Number of rows after filtering.
    size_t VisibleCount() const noexcept { return visible_.size(); }
    // Number of entries before filtering.
    size_t TotalCount() const noexcept { return entries_.size(); }

    // Row accessor. Callers must keep index < VisibleCount(); the reference is
    // invalidated by any mutating call.
    const transport::FileInfo& At(size_t index) const;

    // Counts and byte total over the *visible* rows — what a status line
    // reports, and what changes when the filter changes. A link known to lead
    // to a directory counts as one, so the numbers describe the same two groups
    // the rows are ordered into.
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
    bool PassesFilter(size_t entryIndex) const;

    // The same question as IsDirectoryLike, asked of an unfiltered entry.
    // Sorting and filtering work in entry indices; only the public accessors
    // speak in rows.
    bool EntryIsDirectoryLike(size_t entryIndex) const;

    std::string                      path_;
    std::vector<transport::FileInfo> entries_;
    transport::FsError               error_;

    // What each entry's link leads to, parallel to entries_ and replaced
    // wholesale with them. A parallel vector rather than a field on FileInfo:
    // that struct is what an adapter returns, and this is derived state a
    // second round trip produced.
    std::vector<LinkTarget> linkTargets_;

    // Indices into entries_, in display order. Indices rather than copies so a
    // re-sort never duplicates the entry data.
    std::vector<size_t> visible_;
    size_t   visibleDirs_  = 0;
    uint64_t visibleBytes_ = 0;

    bool        showHidden_       = kDefaultShowHidden;
    std::string nameFilter_;
    SortKey     sortKey_          = kDefaultSortKey;
    SortOrder   sortOrder_        = kDefaultSortOrder;
    bool        directoriesFirst_ = true;
};

// --- Pattern matching, exposed for testing ---------------------------------

// Case-insensitive glob over bytes. Supports '*' (any run, including empty)
// and '?' (exactly one byte). Everything else matches literally.
bool GlobMatch(const std::string& pattern, const std::string& text);

// True when pattern contains a glob metacharacter.
bool IsGlobPattern(const std::string& pattern) noexcept;

} // namespace term::fs
