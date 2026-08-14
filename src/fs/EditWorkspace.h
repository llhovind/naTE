#pragma once
#include <functional>
#include <optional>
#include <string>

namespace term::fs {

// The local tree of remote-edit working copies: where one goes, how it is taken
// away, and how the ones a crash left behind are reclaimed.
//
// Root of the tree. Nothing outside it is ever written or removed on the edit
// path, and every bound in this module is expressed against it.
inline constexpr const char* kEditWorkspaceRoot = "/tmp/nate-edit";

// Where one working copy lives, as the two halves the caller treats differently.
//
// The directory is a template rather than a path because it must be unique per
// open, and only the filesystem can promise that: two edits of one remote file
// derive the same name, and letting them share a file means one download
// truncating a copy another editor is still watching — which uploads the
// truncation straight back to the remote.
//
// The file inside it keeps the remote basename unchanged. That is what the
// editor shows in its title bar and what its syntax highlighting keys off, so
// the uniqueness has to go in the directory and nowhere else.
struct WorkingCopyPath {
    std::string dirTemplate;  // mkdtemp(3) template — "<pid>-XXXXXX" leaf
    std::string fileName;     // basename of the remote path; empty if it has none
};

// Builds the pair for a remote file on a given host. Pure: creates nothing and
// touches no filesystem.
//
// ownerPid is stamped into the directory name rather than a marker file inside
// it, so reclaiming an orphan needs no extra I/O, the directory still holds
// exactly the working copy — which is what lets removal prune by emptiness —
// and anyone looking in /tmp can see who owns what.
//
// fileName is empty when remotePath names no file (empty, or only separators).
// The caller must reject that rather than joining it, since the result would
// name the directory itself.
WorkingCopyPath MakeWorkingCopyPath(const std::string& hostname,
                                    const std::string& remotePath,
                                    int ownerPid);

// Reads the owner pid back out of a working-copy directory's leaf name.
// Nullopt when the name was not produced by MakeWorkingCopyPath — which is the
// safe answer, since an unrecognised directory is one this module must not
// claim the authority to delete.
std::optional<int> OwnerPidOfWorkingCopyDir(const std::string& dirName);

// Removes a working copy and every directory the removal leaves empty, up to
// but never including kEditWorkspaceRoot. Bounded there so a path built
// elsewhere — or a bug in the scheme above — cannot climb out of this tree.
//
// Best-effort by design: a directory another edit is still using stops the
// walk, and nothing here reports failure, because a temp file that outlives its
// session is untidy rather than wrong. A localPath outside the root is ignored.
void RemoveWorkingCopy(const std::string& localPath);

// Answers whether the naTE instance that owns a working copy is still running.
// Injected so the sweep below can be tested without real processes.
using OwnerIsLiveFn = std::function<bool(int pid)>;

// Deletes every working copy whose owning process is gone, and returns how many
// directories that was.
//
// Call once at startup, before any working copy of this run exists: the default
// liveness test below relies on that to recognise a stale directory carrying
// our own recycled pid.
//
// A live owner's tree is left completely alone. That is the whole difficulty
// here — naTE runs multiple instances, and a sweep that deleted whatever it did
// not recognise would destroy another window's working copies with the user's
// unsaved edits still in them. Anything unrecognised is therefore also left
// alone; this reclaims what it can prove is dead, nothing more.
size_t PurgeOrphanedWorkingCopies(const OwnerIsLiveFn& isLive);

// Default liveness test: true when pid names a running process that is another
// instance of this program.
//
// Comparing the process name and not merely existence is what makes this safe
// against pid reuse — after a crash the number may well have been handed to
// something unrelated, and treating that as a live owner would strand the
// working copy forever. Our own pid reads as dead because nothing of this run
// can have written a directory yet at the point this is called.
bool DefaultOwnerIsLive(int pid);

} // namespace term::fs
