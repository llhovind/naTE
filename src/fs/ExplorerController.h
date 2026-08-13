#pragma once
#include "fs/DirModel.h"
#include "fs/Dispatcher.h"
#include "fs/LinkResolver.h"
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace term::fs {

// Signals state changes. Carries no payload beyond what is needed to route the
// notification: the view reads current values from the controller, so nothing
// pushed here can go stale before it is drawn.
class IExplorerListener {
public:
    virtual ~IExplorerListener() = default;
    virtual void OnExplorerLoadingChanged(bool /*loading*/) {}
    virtual void OnExplorerContentsChanged() {}
    virtual void OnExplorerPathChanged(const std::string& /*path*/) {}
};

// What activating a row turned out to mean.
enum class ActivationResult {
    Navigated,  // it was a directory (or a link to one) and we moved into it
    IsFile,     // it is a file; fullPath is where it lives
    Failed,     // it could not be resolved — a broken link, most often
};

using ActivationCallback =
    std::function<void(ActivationResult, std::string fullPath, transport::FsError)>;

// Outcome of a write. Fires exactly once, on the owning thread.
using WriteCallback = std::function<void(transport::FsError)>;

// Navigation state for one remote directory view: where we are, how we got
// here, and what is in it.
//
// Wx-free and single-threaded, with every transport callback routed back
// through the dispatcher. The view owns presentation; this owns "what is the
// current directory and what does it contain".
class ExplorerController {
public:
    ExplorerController(transport::IRemoteFileSystem& remote, Dispatcher dispatch);
    ~ExplorerController();

    ExplorerController(const ExplorerController&)            = delete;
    ExplorerController& operator=(const ExplorerController&) = delete;

    void SetListener(IExplorerListener* listener) { listener_ = listener; }

    DirModel&       Model() noexcept       { return model_; }
    const DirModel& Model() const noexcept { return model_; }

    const std::string& CurrentPath() const noexcept { return currentPath_; }
    bool IsLoading() const noexcept { return loading_; }

    // Navigates to path, canonicalising it server-side first when it is
    // relative or contains "." / ".." / "~".
    void NavigateTo(const std::string& path);
    void Refresh();
    void NavigateUp();

    bool CanGoBack() const noexcept { return historyPos_ > 0; }
    bool CanGoForward() const noexcept { return historyPos_ + 1 < history_.size(); }
    void GoBack();
    void GoForward();

    // Resolves what activating a row means and acts on it. Directories are
    // entered; files are reported back. A symlink costs one stat to find out
    // which it is — listings deliberately do not resolve links, because that
    // would be a round trip per row.
    void Activate(size_t visibleIndex, ActivationCallback onDone);

    // Absolute path of a visible row. Empty when the index is out of range.
    std::string PathOf(size_t visibleIndex) const;

    // -------------------------------------------------------------------------
    // Writes
    //
    // Every one takes a *leaf name*, never a path, and anchors it against the
    // directory currently being shown. That is deliberate: a caller cannot
    // name the wrong directory, so an operation can never land somewhere the
    // user was not looking — including when a navigation completes while a
    // dialog asking for the name is still open.
    //
    // None of them reload; the caller decides what to do with the outcome,
    // because whether a refresh is worth a round trip is a view's business.
    // -------------------------------------------------------------------------

    void CreateDirectory(const std::string& name, WriteCallback onDone);

    // Renames within the current directory.
    //
    // The destination is checked before the rename is issued. POSIX rename()
    // replaces an existing file without a word and only some SFTP servers
    // decline, so without this a rename onto an existing name is silent data
    // loss. Reports AlreadyExists instead.
    void RenameEntry(const std::string& oldName, const std::string& newName,
                     WriteCallback onDone);

    void SetEntryPermissions(const std::string& name, uint32_t mode,
                             WriteCallback onDone);

    // True when path needs the server's realpath before it can be listed.
    // Exposed for testing; a clean absolute path can be listed directly, which
    // halves the round trips for ordinary click-through browsing.
    static bool NeedsCanonicalisation(const std::string& path);

    // Checks a name the user typed for one directory entry. Rejects the empty
    // string, anything containing '/', and "." / ".." — all of which a server
    // would either refuse or, worse, quietly reinterpret as a different path.
    // Returns FsErrorCode::InvalidName with a displayable message, or success.
    static transport::FsError ValidateLeafName(const std::string& name);

private:
    // Where a completed navigation should leave the history cursor.
    struct HistoryIntent {
        enum class Kind { Push, Goto, None };
        Kind   kind  = Kind::Push;
        size_t index = 0;   // Goto only
    };

    void Begin(const std::string& path, HistoryIntent intent);
    void RequestList(const std::string& path, uint64_t generation, HistoryIntent intent);
    void OnListed(uint64_t generation, std::string path, HistoryIntent intent,
                  std::vector<transport::FileInfo> entries, transport::FsError err);
    void SetLoading(bool loading);

    // Asks what the listing's links point at, and folds the answers back into
    // the model when they arrive. Deliberately *after* the rows are published:
    // the listing is the thing the user is waiting for, and holding it back
    // for a per-link round trip would make every directory containing one feel
    // slow. The rows are therefore correct from the start and better ordered a
    // moment later, rather than late and complete.
    void ResolveLinks();

    // Wraps a WriteCallback so the adapter's answer arrives back on the owning
    // thread, guarded. Every write funnels through it rather than repeating the
    // same four-line bounce.
    transport::DoneCallback Bounce(WriteCallback onDone);

    transport::IRemoteFileSystem& remote_;
    DispatchGuard                 guard_;
    LinkResolver                  links_;
    IExplorerListener*            listener_ = nullptr;

    DirModel    model_;
    std::string currentPath_;
    bool        loading_ = false;

    // Bumped by every navigation. A response whose generation no longer
    // matches belongs to a request the user has already moved on from, and is
    // dropped — otherwise a slow listing could overwrite a newer one and drop
    // the user somewhere they did not ask to be.
    uint64_t generation_ = 0;

    std::vector<std::string> history_;
    size_t                   historyPos_ = 0;
};

} // namespace term::fs
