#pragma once
#include "transport/IRemoteFileSystem.h"

#include <string>

namespace term::fs {

// What one finished upload is worth telling the user.
enum class SaveAnnouncement {
    Nothing,   // superseded, or a repeat of what has already been said
    Saved,
    Failed,
};

// Decides which finished uploads an observer hears about.
//
// Pulled out of the upload path because it is the whole of the judgement in it:
// everything else there is bookkeeping, and a rule about what the user is told
// deserves to be readable and exercisable on its own.
//
// Not thread-safe, and deliberately so — one instance belongs to one edit and
// is consulted only on that edit's owning thread.
class SaveAnnouncePolicy {
public:
    // supersededByPending says another save is already queued behind this one.
    //
    // Nothing is announced in that case: the queued upload is about to replace
    // this outcome either way, and a failure that the very next attempt repairs
    // was never the user's problem. It also leaves the policy's memory
    // untouched, so a burst neither arms nor disarms what follows it.
    SaveAnnouncement Decide(const transport::FsError& err,
                            bool supersededByPending);

private:
    // The last failure announced, empty once a save has succeeded. Holding the
    // message and not merely "something failed" is what makes a *different*
    // failure worth raising while a repeat of the same one is not.
    std::string lastReported_;
};

} // namespace term::fs
