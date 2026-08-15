#pragma once
#include <functional>

namespace term::fs {

// Whether the naTE instance that owns a temporary file or directory is still
// running.
//
// naTE runs as several independent processes, and everything it leaves in the
// system temp area is therefore owned by one of them rather than by "naTE".
// Every startup sweep needs the same question answered before it deletes
// anything — one instance reclaiming another's live working files would destroy
// unsaved edits or a transfer in flight — so the test lives here rather than
// beside any one of them.
//
// Injected as a function so a sweep can be tested without real processes.
using OwnerIsLiveFn = std::function<bool(int pid)>;

// Default liveness test: true when pid names a running process that is another
// instance of this program.
//
// Comparing the process name and not merely existence is what makes this safe
// against pid reuse — after a crash the number may well have been handed to
// something unrelated, and treating that as a live owner would strand the file
// forever.
//
// Our own pid reads as dead, which is what lets a sweep reclaim files left by a
// previous run that happened to have the number we now carry. That is only
// sound while nothing of this run has written such a file yet, so every sweep
// relying on it must run at startup and before any work of its own kind begins.
bool DefaultOwnerIsLive(int pid);

} // namespace term::fs
