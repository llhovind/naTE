#pragma once
#include "transport/IRemoteFileSystem.h"

namespace term::transport {

// The machine naTE is running on, behind the same port as a remote one.
//
// Satisfying IRemoteFileSystem is what lets the explorer's controller, model
// and view serve the local pane unchanged — the alternative was a parallel
// implementation of browsing, sorting and filtering that would drift.
//
// Threading: every operation completes synchronously and invokes its callback
// before returning. There is no worker thread to defer to, and inventing one
// would buy nothing for a local disk. Callers that marshal (as
// ExplorerController does) are unaffected. The caveat is a network mount: an
// unresponsive NFS or SSHFS path will block the calling thread, which for the
// UI means a stall. That is a deliberate trade for the simplicity, and the
// reason the file explorer exists in the first place is to avoid needing such
// mounts.
//
// Transfers are not supported here. Download and Upload move bytes between
// this machine and a *remote*, which is the remote adapter's job; a local copy
// is a different operation and no caller needs one yet.
class LocalFileSystem final : public IRemoteFileSystem {
public:
    bool IsLocalDisk() const noexcept override { return true; }

    void List(const std::string& path, ListCallback onDone) override;
    void RealPath(const std::string& path, PathCallback onDone) override;
    void Stat(const std::string& path, StatCallback onDone) override;
    void ReadLink(const std::string& path, PathCallback onDone) override;
    void MakeDirectory(const std::string& path, uint32_t mode,
                       DoneCallback onDone) override;
    void Remove(const std::string& path, bool isDir, DoneCallback onDone) override;
    void Rename(const std::string& from, const std::string& to,
                DoneCallback onDone) override;
    void SetPermissions(const std::string& path, uint32_t mode,
                        DoneCallback onDone) override;

    TransferHandle Download(const std::string& remotePath,
                            const std::string& localPath,
                            ProgressCallback onProgress,
                            DoneCallback onDone) override;
    TransferHandle Upload(const std::string& localPath,
                          const std::string& remotePath,
                          ProgressCallback onProgress,
                          DoneCallback onDone) override;
    void Cancel(TransferHandle handle) override;
};

} // namespace term::transport
