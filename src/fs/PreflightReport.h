#pragma once
#include "fs/SpaceForecast.h"
#include "transport/IRemoteFileSystem.h"

#include <cstddef>
#include <string>

namespace term::fs {

// Everything the queue worked out about a batch before starting it.
//
// Space is one finding among several rather than the whole story, and keeping
// them in one report is what lets a single decision point cover them: the user
// is being asked "should this copy go ahead", and a copy can be a bad idea
// because it will not fit *or* because part of what they selected could not be
// read. Answering those in two separate dialogs would ask them twice about one
// transfer.
struct PreflightReport {
    SpaceForecast forecast;

    // Directories the walk could not list and skipped over. Their contents are
    // not in the batch at all, so a copy that proceeds will be missing them —
    // which is the one outcome a user must never discover afterwards, because
    // a partial copy is indistinguishable from a complete one once it finishes.
    size_t unreadableDirectories = 0;

    // Why the first of them failed, and which one it was.
    //
    // Carried rather than merely counted because "3 directories could not be
    // read" tells a user that something is wrong without telling them anything
    // they can act on. The server's own reason, against the path it refused,
    // is the difference between a dead end and a fixable problem.
    transport::FsError firstFailure;
    std::string        firstFailurePath;

    bool CopyWillBeIncomplete() const noexcept
    {
        return unreadableDirectories > 0;
    }

    // Whether there is anything worth stopping to tell the user about.
    bool Concerning() const noexcept
    {
        return forecast.Concerning() || CopyWillBeIncomplete();
    }
};

} // namespace term::fs
