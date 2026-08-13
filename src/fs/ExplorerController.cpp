#include "fs/ExplorerController.h"
#include "fs/FileMode.h"
#include "fs/RemotePath.h"

namespace term::fs {

ExplorerController::ExplorerController(transport::IRemoteFileSystem& remote,
                                       Dispatcher dispatch)
    : remote_(remote)
    , guard_(dispatch)
    , links_(remote, std::move(dispatch))
{}

// Callbacks still held by the transport check the guard before touching us,
// so destruction must happen on the same thread the dispatcher posts to.
ExplorerController::~ExplorerController() = default;

bool ExplorerController::NeedsCanonicalisation(const std::string& path)
{
    if (path.empty()) return true;
    // "~" is shell syntax the server expands; we cannot resolve it locally.
    if (path.find('~') != std::string::npos) return true;
    if (!path::IsAbsolute(path)) return true;
    // Already-normalised absolute paths have nothing left for realpath to do
    // except follow symlinks, and following them here would fight the user:
    // they clicked a link, so the link's path is the one to show.
    return path::Normalise(path) != path;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

void ExplorerController::NavigateTo(const std::string& path)
{
    Begin(path, HistoryIntent{HistoryIntent::Kind::Push, 0});
}

void ExplorerController::Refresh()
{
    if (currentPath_.empty()) return;
    Begin(currentPath_, HistoryIntent{HistoryIntent::Kind::None, 0});
}

void ExplorerController::NavigateUp()
{
    if (currentPath_.empty()) return;
    // Appending ".." and normalising handles every case uniformly: the root
    // stays at the root, and a relative "." becomes "..", which the server
    // resolves against the login directory.
    NavigateTo(path::Normalise(path::Join(currentPath_, "..")));
}

void ExplorerController::GoBack()
{
    if (!CanGoBack()) return;
    const size_t target = historyPos_ - 1;
    Begin(history_[target], HistoryIntent{HistoryIntent::Kind::Goto, target});
}

void ExplorerController::GoForward()
{
    if (!CanGoForward()) return;
    const size_t target = historyPos_ + 1;
    Begin(history_[target], HistoryIntent{HistoryIntent::Kind::Goto, target});
}

void ExplorerController::Begin(const std::string& path, HistoryIntent intent)
{
    const uint64_t generation = ++generation_;
    SetLoading(true);
    // Dropped here rather than when the new listing lands: those lookups are
    // queued on the same connection the listing has to travel over, and they
    // describe a directory the user has already left.
    links_.Cancel();

    if (!NeedsCanonicalisation(path)) {
        RequestList(path, generation, intent);
        return;
    }

    auto ctx = guard_.For(this);
    remote_.RealPath(path, [ctx, generation, intent, path](
                               std::string resolved, transport::FsError err) {
        ctx.Post([generation, intent, path, resolved = std::move(resolved),
                  err = std::move(err)](ExplorerController& c) mutable {
            if (generation != c.generation_) return;   // superseded
            // A server that cannot canonicalise still deserves an attempt at
            // the literal path: realpath fails on a directory you may lack
            // execute permission to resolve but can still list.
            c.RequestList(err.Failed() ? path : resolved, generation, intent);
        });
    });
}

void ExplorerController::RequestList(const std::string& path, uint64_t generation,
                                     HistoryIntent intent)
{
    auto ctx = guard_.For(this);
    remote_.List(path, [ctx, generation, intent, path](
                           std::vector<transport::FileInfo> entries,
                           transport::FsError err) {
        ctx.Post([generation, intent, path, entries = std::move(entries),
                  err = std::move(err)](ExplorerController& c) mutable {
            c.OnListed(generation, path, intent, std::move(entries), std::move(err));
        });
    });
}

void ExplorerController::OnListed(uint64_t generation, std::string path,
                                  HistoryIntent intent,
                                  std::vector<transport::FileInfo> entries,
                                  transport::FsError err)
{
    if (generation != generation_) return;   // superseded by a newer navigation

    const bool pathChanged = path != currentPath_;
    currentPath_ = path;

    // History commits here rather than at request time so that a navigation
    // the user abandons mid-flight leaves no trace in the back stack.
    switch (intent.kind) {
        case HistoryIntent::Kind::Push:
            history_.resize(historyPos_ + (history_.empty() ? 0 : 1));
            history_.push_back(path);
            historyPos_ = history_.size() - 1;
            break;
        case HistoryIntent::Kind::Goto:
            if (intent.index < history_.size()) historyPos_ = intent.index;
            break;
        case HistoryIntent::Kind::None:
            break;
    }

    model_.SetPath(path);
    // A listing that failed partway still delivers its rows; DirModel keeps
    // both so the view can show the contents and say the result is incomplete.
    model_.SetEntries(std::move(entries), std::move(err));

    SetLoading(false);
    if (pathChanged && listener_) listener_->OnExplorerPathChanged(currentPath_);
    if (listener_) listener_->OnExplorerContentsChanged();

    ResolveLinks();
}

void ExplorerController::ResolveLinks()
{
    std::vector<std::string> names = model_.UnresolvedLinkNames();
    // A directory without links owes nothing — the overwhelmingly common case,
    // and the one that must stay free.
    if (names.empty()) {
        links_.Cancel();
        return;
    }

    auto ctx = guard_.For(this);
    const std::string directory = currentPath_;
    const uint64_t    generation = generation_;

    links_.Resolve(directory, std::move(names),
                   [ctx, generation](std::vector<LinkResolution> resolutions) {
        ctx.Post([generation, resolutions = std::move(resolutions)](
                     ExplorerController& c) mutable {
            // The resolver drops a superseded batch itself; this second check
            // covers the navigation that started *and finished* while the
            // batch was in flight, which would otherwise apply one listing's
            // answers to another listing's rows.
            if (generation != c.generation_) return;
            c.model_.ApplyLinkTargets(resolutions);
            if (c.listener_) c.listener_->OnExplorerContentsChanged();
        });
    });
}

void ExplorerController::SetLoading(bool loading)
{
    if (loading_ == loading) return;
    loading_ = loading;
    if (listener_) listener_->OnExplorerLoadingChanged(loading_);
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

std::string ExplorerController::PathOf(size_t visibleIndex) const
{
    if (visibleIndex >= model_.VisibleCount()) return {};
    return path::Join(currentPath_, model_.At(visibleIndex).name);
}

void ExplorerController::Activate(size_t visibleIndex, ActivationCallback onDone)
{
    if (visibleIndex >= model_.VisibleCount()) {
        if (onDone)
            onDone(ActivationResult::Failed, {},
                   transport::FsError::Make(transport::FsErrorCode::NoSuchFile,
                                            "No such row"));
        return;
    }

    const transport::FileInfo& entry = model_.At(visibleIndex);
    const std::string full = PathOf(visibleIndex);

    if (entry.isDir) {
        NavigateTo(full);
        if (onDone) onDone(ActivationResult::Navigated, full, {});
        return;
    }


    if (!entry.isSymlink) {
        if (onDone) onDone(ActivationResult::IsFile, full, {});
        return;
    }

    // Stat follows the link, so one round trip settles whether this is a
    // doorway or a file — and reports a broken link as the error it is.
    auto ctx = guard_.For(this);
    remote_.Stat(full, [ctx, full, onDone](transport::FileInfo info,
                                           transport::FsError err) {
        ctx.Post([full, onDone, info, err = std::move(err)](
                     ExplorerController& c) mutable {
            if (err.Failed()) {
                if (onDone) onDone(ActivationResult::Failed, full, std::move(err));
                return;
            }
            if (info.isDir) {
                c.NavigateTo(full);
                if (onDone) onDone(ActivationResult::Navigated, full, {});
                return;
            }
            if (onDone) onDone(ActivationResult::IsFile, full, {});
        });
    });
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------

transport::FsError ExplorerController::ValidateLeafName(const std::string& name)
{
    const auto reject = [](std::string message) {
        return transport::FsError::Make(transport::FsErrorCode::InvalidName,
                                        std::move(message));
    };

    if (name.empty())
        return reject("A name cannot be empty.");
    if (name.find('/') != std::string::npos)
        return reject("A name cannot contain '/'.");
    // Every directory already has these two, and a server would resolve them
    // rather than treat them as the name the user meant.
    if (name == "." || name == "..")
        return reject("'" + name + "' is not a name a file can have.");

    return transport::FsError::Success();
}

transport::DoneCallback ExplorerController::Bounce(WriteCallback onDone)
{
    auto ctx = guard_.For(this);
    return [ctx, onDone = std::move(onDone)](transport::FsError err) {
        ctx.Post([onDone, err = std::move(err)](ExplorerController&) mutable {
            if (onDone) onDone(std::move(err));
        });
    };
}

void ExplorerController::CreateDirectory(const std::string& name, WriteCallback onDone)
{
    if (const transport::FsError err = ValidateLeafName(name); err.Failed()) {
        if (onDone) onDone(err);
        return;
    }

    remote_.MakeDirectory(path::Join(currentPath_, name), kDefaultDirectoryMode,
                          Bounce(std::move(onDone)));
}

void ExplorerController::RenameEntry(const std::string& oldName,
                                     const std::string& newName,
                                     WriteCallback onDone)
{
    if (const transport::FsError err = ValidateLeafName(newName); err.Failed()) {
        if (onDone) onDone(err);
        return;
    }
    // Renaming something to what it is already called is not a failure, and is
    // not worth a round trip either.
    if (newName == oldName) {
        if (onDone) onDone(transport::FsError::Success());
        return;
    }

    // Both ends anchored to the directory as it is now, so a navigation that
    // lands mid-operation cannot turn this into a move.
    const std::string from = path::Join(currentPath_, oldName);
    const std::string to   = path::Join(currentPath_, newName);

    auto ctx = guard_.For(this);
    remote_.Stat(to, [ctx, from, to, newName, onDone](transport::FileInfo,
                                                      transport::FsError statErr) {
        ctx.Post([from, to, newName, onDone,
                  statErr = std::move(statErr)](ExplorerController& c) mutable {
            // Only a definite "nothing there" clears the way. Any other error
            // is the destination telling us something, and renaming onto it
            // would be guessing with the user's data.
            if (statErr.code != transport::FsErrorCode::NoSuchFile) {
                if (onDone)
                    onDone(transport::FsError::Make(
                        transport::FsErrorCode::AlreadyExists,
                        "'" + newName + "' already exists in this directory."));
                return;
            }
            c.remote_.Rename(from, to, c.Bounce(std::move(onDone)));
        });
    });
}

void ExplorerController::SetEntryPermissions(const std::string& name, uint32_t mode,
                                             WriteCallback onDone)
{
    remote_.SetPermissions(path::Join(currentPath_, name), mode,
                           Bounce(std::move(onDone)));
}

} // namespace term::fs
