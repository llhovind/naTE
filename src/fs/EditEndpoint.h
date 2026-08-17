#pragma once
#include "transport/IRemoteFileSystem.h"

#include <string>

namespace term::fs {

// The remote side of one edit: the filesystem the file lives on, and a name for
// it to put in front of the user.
//
// Carries no session identity, exactly as TransferEndpoint does not — a
// filesystem is the whole of what this layer needs to move the file, and an id
// would only be an invitation to reach back into session/ for the rest of it.
// Whoever owns the identity resolves it at the UI boundary, in both directions:
// down to build one of these, and back up when a save reports which endpoint it
// landed on.
//
// The pointer is non-owning and lives as long as the transport behind it. An
// edit therefore has to be stopped when its session goes away, which is what
// RemoteEditManager::StopEditsForFilesystem is for.
struct EditEndpoint {
    transport::IRemoteFileSystem* fs = nullptr;
    // Shown beside the remote path, so the user can tell two edits of the same
    // filename on different hosts apart. May be empty.
    std::string                   label;

    bool Valid() const noexcept { return fs != nullptr; }
};

} // namespace term::fs
