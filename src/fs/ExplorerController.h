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
    // Every one names its subject by *leaf name*, never by path, anchored
    // against the directory currently being shown. That is deliberate: a
    // caller cannot name the wrong subject, so an operation can never act on
    // something the user was not looking at — including when a navigation
    // completes while a dialog asking for the name is still open.
    //
    // None of them reload; the caller decides what to do with the outcome,
    // because whether a refresh is worth a round trip is a view's business.
    // -------------------------------------------------------------------------

    void CreateDirectory(const std::string& name, WriteCallback onDone);

    // Renames within the current directory, or moves anywhere else on the same
    // filesystem — one operation, because that is what the remote offers:
    // SSH_FXP_RENAME takes two arbitrary paths and POSIX rename() is the move
    // primitive. Only the destination arithmetic differs.
    //
    // destination may be a leaf name, a relative path, or an absolute one.
    // Relative destinations are resolved here, against the directory on screen
    // — the server would resolve them against the login directory instead, so
    // leaving that to the remote would land "../archive/f" somewhere the user
    // never named. "~" cannot be resolved without a round trip against a path
    // that does not exist yet, and is refused rather than guessed at.
    //
    // A destination that is an existing directory receives the entry under its
    // own name, as `mv` does. That redirect happens at most once. A trailing
    // '/' says the user meant a directory and nothing else will do — without
    // that rule, one mistyped folder name silently produces a file called
    // "archive" where "archive/" was asked for.
    //
    // The destination is checked before the rename is issued. POSIX rename()
    // replaces an existing file without a word, only some SFTP servers
    // decline, and SFTP v3's status vocabulary is too coarse to tell a
    // collision from any other failure — so without this check a rename onto
    // an existing name is silent data loss. Reports AlreadyExists instead.
    //
    // Both ends are resolved before anything is sent, so a navigation landing
    // mid-operation cannot redirect it.
    void RenameEntry(const std::string& oldName, const std::string& destination,
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
    // What it means for the destination to be — or not to be — a directory.
    //
    // Absorbs:     one that exists takes the entry under its own name, as `mv`
    //              does; anything else is an ordinary destination path.
    // Required:    the user wrote a trailing '/', so only a directory will do.
    // IsInTheWay:  a directory is just a collision. This bounds the absorb to
    //              one hop: moving "d1" onto "d2" when "d2/d1" already exists
    //              must fail rather than descend again and land at "d2/d1/d1".
    enum class DirectoryDestination { Absorbs, Required, IsInTheWay };

    // Shared tail of every rename: confirm nothing is in the way, then issue
    // it. Both paths are absolute and already resolved.
    void RenameResolved(std::string from, std::string to,
                        DirectoryDestination onDirectory, WriteCallback onDone);

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
