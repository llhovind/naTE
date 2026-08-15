#pragma once
#include "fs/OwnerLiveness.h"
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <optional>
#include <string>

namespace term::fs {

// The local staging area for remote-to-remote transfers.
//
// SFTP moves bytes only between a server and this machine, never server to
// server, so a copy between two remote hosts is a download followed by an
// upload with a local file in the middle. That file is the whole of what this
// module is about: where it goes, what it is called, and how the ones a crash
// left behind are reclaimed.
//
// It matters more than its size suggests. A staging file holds an entire
// transferred file — there is no streaming — so it is routinely the largest
// thing naTE writes anywhere, and one abandoned by a killed process is a
// permanent occupant of a volume the user is likely to be short of.

// Directory the staging files live in.
//
// Under the system temp directory rather than a hardcoded /tmp, so TMPDIR
// still means what it means everywhere else. That is the escape hatch for the
// case this module runs into most: /tmp is a tmpfs capped at a fraction of RAM
// on most current distributions, which is smaller than the files being relayed
// through it, and pointing TMPDIR at a real disk has to be enough to fix that.
std::string RelayRoot();

// Creates RelayRoot() if it is not already there, owner-accessible only.
// Returns false when it could not be created or is not a directory.
//
// The 0700 matters: a staging file carries the source file's own permissions,
// so a file the user keeps at 0600 stays at 0600 on the way past — but that
// only holds for the file. Anything in a world-readable directory can at least
// be enumerated, and the names of the files someone is moving between two
// servers are worth keeping to themselves.
bool EnsureRelayRoot();

// Names the staging file for one transfer. Pure: creates nothing.
//
// ownerPid leads the name so an orphan sweep can tell whose file it is looking
// at without opening it. jobId and stamp follow because a pid alone is not
// unique — one process runs several transfer queues, each numbering its jobs
// from one, so two windows relaying at once would otherwise collide on a name
// and each truncate the other's file.
//
// leaf is the source's basename, kept only so a large file found in the temp
// directory can be recognised. It is sanitised and truncated: it comes from a
// remote server, and a name from there is bytes rather than something this
// module may assume is safe to build a local path out of.
std::string MakeRelayPath(const std::string& leaf, uint64_t jobId,
                          int ownerPid, uint64_t stamp);

// Space on the volume holding RelayRoot(), or nullopt when it cannot be read —
// which includes the ordinary case of the directory not existing yet, before
// this run has staged anything.
//
// Synchronous, unlike every other space query in the system. This is a local
// directory and the call is a syscall rather than a round trip, so the usual
// reason to defer does not apply; the caveat is the same one LocalFileSystem
// carries, that a temp directory on an unresponsive network mount would block
// the calling thread. TMPDIR pointing at such a mount is pathological enough to
// accept rather than build machinery against.
std::optional<transport::FsSpaceInfo> RelayVolumeSpace();

// Reads the owner pid back out of a staging file's name. Nullopt when the name
// was not produced by MakeRelayPath — the safe answer, since a file this module
// does not recognise is not one it may claim the authority to delete.
std::optional<int> OwnerPidOfRelayFile(const std::string& fileName);

// Deletes every staging file whose owning process is gone, and returns how many
// that was.
//
// Call once at startup, before any transfer of this run can have begun — see
// DefaultOwnerIsLive for why that ordering is what makes reclaiming safe.
//
// A live owner's files are left alone, and so is anything unrecognised: naTE
// runs several instances, and deleting another one's staging file would abort a
// transfer in progress. This reclaims what it can prove is dead, nothing more.
size_t PurgeOrphanedRelayFiles(const OwnerIsLiveFn& isLive);

} // namespace term::fs
