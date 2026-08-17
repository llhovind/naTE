#pragma once
#include <optional>
#include <string>

namespace term::fs {

// What naTE leaves in the system temp area, and how it claims it.
//
// Two things live here rather than in either of the modules built on them,
// because both modules encode the same two rules and a change to either would
// otherwise need two edits. EditWorkspace keeps remote-edit working copies;
// RelayWorkspace keeps the staging files a server-to-server transfer passes
// through. Neither owns the scheme they share:
//
//   - an entry's name begins with the pid of the instance that created it, so a
//     startup sweep can tell whose it is without opening it (see OwnerLiveness
//     for the other half of that question), and
//   - the directory holding those entries is private to this user.
//
// Pure and dependency-free: no transport, no wx, nothing above it.

// Divides the owner pid from the rest of a temp entry's name.
inline constexpr char kOwnerSeparator = '-';

// Creates path as a directory only this user may reach, or accepts one that is
// already there. False when it could not be created, or when what is already
// there must not be trusted.
//
// The mode is the point. mkdir(2) rather than std::filesystem::create_directory
// because the standard call applies 0777 narrowed by the umask, and a umask
// permissive enough to leave this group- or world-readable is exactly the
// configuration this defends against. 0700 has no group or other bits to begin
// with, so no umask can widen or narrow the result.
//
// An existing directory is checked rather than assumed, which is the half that
// is easy to miss: a temp path is a name any local user can win the race to
// create first, and one they own is one they can rename, empty, or watch the
// contents of. It is therefore rejected unless it is a real directory — lstat,
// so a symlink pointing somewhere else is caught rather than followed — owned
// by this user. One that *is* ours but too permissive is tightened rather than
// refused: we own it, and refusing would strand anyone whose directory was
// created by a run that predates this check.
bool EnsurePrivateDirectory(const std::string& path);

// Reads the owner pid back out of a temp entry's name. Nullopt when the name
// does not carry one — the safe answer, since an entry this scheme does not
// recognise is not one any sweep may claim the authority to delete.
std::optional<int> OwnerPidOfTaggedName(const std::string& name);

} // namespace term::fs
