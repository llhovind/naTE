#pragma once
#include "fs/Dispatcher.h"
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace term::fs {

// One removal, in the order it must happen.
struct DeleteStep {
    std::string path;
    bool        isDir = false;
    int         depth = 0;
};

// What deleting a path will actually do, enumerated before anything is
// removed.
//
// Planning and executing are separate phases on purpose: the user is shown a
// count and then that exact plan is carried out, so the confirmation can never
// describe something different from what happens.
struct DeletePlan {
    // Post-order: children always precede their parent, because a directory
    // cannot be removed until it is empty.
    std::vector<DeleteStep> steps;

    size_t   fileCount  = 0;
    size_t   dirCount   = 0;
    uint64_t totalBytes = 0;

    // Set when part of the tree could not be enumerated. The plan still holds
    // what *was* readable; a caller must decide whether to proceed on a
    // partial view, and the UI says so plainly.
    transport::FsError error;

    bool Empty() const noexcept { return steps.empty(); }
};

// One thing the user asked to delete, as the listing described it.
//
// size carries the byte count the caller already has in hand, so a plan for a
// plain file can report an honest total rather than zero — the confirmation
// dialog quotes that number, and it must not understate what is about to go.
struct DeleteTarget {
    std::string path;
    bool        isDir     = false;
    bool        isSymlink = false;
    uint64_t    size      = 0;
};

// Enumerates and removes remote files and directory trees.
//
// Recursive removal is composed here from single unlink/rmdir calls rather
// than sent as `rm -rf` down the shell channel. Shelling out would bypass the
// count shown to the user, the symlink rule below, and the ability to stop —
// and would run in whatever state the shell happened to be in.
//
// Symlinks are removed as links and never followed. Deleting a link to /etc
// must delete the link, not the contents of /etc.
class RemoteDeleter {
public:
    RemoteDeleter(transport::IRemoteFileSystem& remote, Dispatcher dispatch);
    ~RemoteDeleter();

    RemoteDeleter(const RemoteDeleter&)            = delete;
    RemoteDeleter& operator=(const RemoteDeleter&) = delete;

    // Builds the plan for removing one target. Its flags come from the listing
    // the user is looking at, so a symlinked directory is planned as a single
    // unlink rather than walked.
    void Plan(const DeleteTarget& target, std::function<void(DeletePlan)> onDone);

    // Plans several targets as one operation, so a multi-row selection
    // produces a single count and a single confirmation rather than one
    // dialog per file. Targets are enumerated in order and their steps
    // concatenated; because the targets are siblings, finishing one before
    // starting the next keeps every child ahead of its parent.
    void Plan(std::vector<DeleteTarget> targets,
              std::function<void(DeletePlan)> onDone);

    // Executes a plan step by step, stopping at the first failure — a tree
    // that cannot be emptied cannot be removed, so pressing on would only
    // produce a cascade of misleading errors about non-empty directories.
    // onProgress reports completed-step counts; either callback may be empty.
    void Execute(DeletePlan plan,
                 std::function<void(size_t completed, size_t total)> onProgress,
                 std::function<void(transport::FsError)> onDone);

private:
    transport::IRemoteFileSystem& remote_;
    DispatchGuard                 guard_;
};

// Upper bound on recursion depth, shared with the transfer walker's reasoning:
// symlinks are already excluded, so this only guards against a pathologically
// deep tree or a server reporting a directory that contains itself.
inline constexpr int kMaxDeleteDepth = 64;

} // namespace term::fs
