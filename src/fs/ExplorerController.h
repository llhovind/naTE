#pragma once
#include "fs/DirModel.h"
#include "fs/Dispatcher.h"
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

    // True when path needs the server's realpath before it can be listed.
    // Exposed for testing; a clean absolute path can be listed directly, which
    // halves the round trips for ordinary click-through browsing.
    static bool NeedsCanonicalisation(const std::string& path);

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

    transport::IRemoteFileSystem& remote_;
    DispatchGuard                 guard_;
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
