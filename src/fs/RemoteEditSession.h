#pragma once
#include "fs/Dispatcher.h"
#include "fs/EditEndpoint.h"
#include "fs/SaveAnnouncePolicy.h"
#include "transport/IRemoteFileSystem.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace term::fs {

// An upload that did not reach the remote. Carries the local path because the
// edits are still there and still recoverable — that is the one thing the user
// needs to be told beyond the fact that the save did not land.
struct SaveFailure {
    // Which endpoint the save was for. The owner routes the report by it: the
    // window hosting that connection is the one that should raise it.
    transport::IRemoteFileSystem* fs = nullptr;
    std::string                   remotePath;
    std::string                   localPath;
    std::string                   message;
};

// Owns one active remote-edit round-trip:
//   download → editor launch → inotify watch → coalesced SFTP upload on each save.
//
// Thread-safety contract:
//   All public methods except Stop() must be called on the owning thread — the
//   one the Dispatcher posts to.
//   Stop() is safe to call from any thread.
//   Destructor calls Stop(); never destroy from a background thread while the
//   watch loop might be calling back into the manager.
class RemoteEditSession {
public:
    // endpoint.fs must be non-null: a session with nowhere to upload to could
    // only fail every save, and rejecting the open is the honest answer. The
    // owner checks it, which is why nothing here re-checks per upload.
    //
    // dispatch marshals worker-thread callbacks onto the thread that owns this
    // session — the UI thread in the application, a drainable executor in tests.
    RemoteEditSession(EditEndpoint endpoint,
                      std::string  remotePath,
                      std::string  localPath,
                      Dispatcher   dispatch);
    ~RemoteEditSession();

    // Non-copyable, non-movable: the dispatch guard is held by callbacks.
    RemoteEditSession(const RemoteEditSession&)            = delete;
    RemoteEditSession& operator=(const RemoteEditSession&) = delete;

    // Reports a save that has reached the remote: the endpoint it landed on and
    // the absolute remote path. Invoked on the owning thread, once per
    // successful upload, so an observer can re-read what the write changed.
    using SavedFn = std::function<void(transport::IRemoteFileSystem*,
                                       const std::string& remotePath)>;
    void SetOnSaved(SavedFn cb) { onSaved_ = std::move(cb); }

    // Reports a save that did NOT reach the remote. Invoked on the owning
    // thread. A silent failure is the dangerous one: the editor reported a
    // clean write, so without this the user believes the remote file was
    // updated. Consecutive failures carrying the same message are reported
    // once — a dropped connection would otherwise raise one dialog per
    // keystroke-save — and a successful upload re-arms reporting.
    using FailedFn = std::function<void(const SaveFailure&)>;
    void SetOnSaveFailed(FailedFn cb) { onSaveFailed_ = std::move(cb); }

    // Starts the inotify watch loop. Must be called once after construction.
    void Start();

    // Stops the watch loop (write to stop-pipe, join thread, close fds), then
    // removes the working copy. Idempotent; safe from any thread.
    void Stop();

    transport::IRemoteFileSystem* GetFileSystem() const { return endpoint_.fs; }
    const std::string& GetHost()       const { return endpoint_.label; }
    const std::string& GetRemotePath() const { return remotePath_;  }
    const std::string& GetLocalPath()  const { return localPath_;   }

private:
    void WatchLoop();
    void TriggerUpload();

    // Applies the announce policy to one finished upload and re-triggers a
    // coalesced save if one arrived while this was in flight. Owning thread only.
    void OnUploadFinished(const transport::FsError& err);

    EditEndpoint  endpoint_;
    std::string   remotePath_;
    std::string   localPath_;
    SavedFn       onSaved_;
    FailedFn      onSaveFailed_;

    // Decides which upload outcomes reach the observer. Owning thread only:
    // consulted solely from the upload continuation.
    SaveAnnouncePolicy announce_;

    // Retires the watch and upload callbacks. Retired by Stop() rather than
    // only by destruction: a stopped session lingers until its owner erases it,
    // and an upload continuation must not fire in that window.
    DispatchGuard guard_;

    std::atomic<bool> uploadInFlight_{false};
    std::atomic<bool> pendingUpload_{false};

    int inotifyFd_    = -1;
    int stopPipe_[2]  = {-1, -1};
    std::thread watchThread_;
};

} // namespace term::fs
