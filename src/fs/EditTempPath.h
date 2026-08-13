#pragma once
#include <string>

namespace term::fs {

// Root under which every remote-edit working copy is created. Nothing outside
// this tree is ever written or removed on the edit path.
inline constexpr const char* kEditTempRoot = "/tmp/nate-edit";

// Where one remote-edit working copy lives on this machine, as the two halves
// the caller has to treat differently.
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
struct EditTempPath {
    std::string dirTemplate;  // mkdtemp(3) template — ends in XXXXXX
    std::string fileName;     // basename of the remote path; empty if it has none
};

// Builds the pair for a remote file on a given host. Pure: creates nothing and
// touches no filesystem.
//
// fileName is empty when remotePath names no file (empty, or only separators).
// The caller must reject that rather than joining it, since the result would
// name the directory itself.
EditTempPath MakeEditTempPath(const std::string& hostname,
                              const std::string& remotePath);

// Removes a working copy and every directory the removal leaves empty, up to
// but never including kEditTempRoot. Bounded there so a path built elsewhere —
// or a bug in the scheme above — cannot climb out of the tree this module owns.
//
// Best-effort by design: a directory another edit is still using stops the
// walk, and nothing here reports failure, because a temp file that outlives its
// session is untidy rather than wrong. A localPath outside kEditTempRoot is
// ignored entirely.
void RemoveWorkingCopy(const std::string& localPath);

} // namespace term::fs
