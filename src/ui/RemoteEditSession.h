#pragma once
#include "fs/Dispatcher.h"
#include "session/SessionManager.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include <wx/app.h>

namespace ui {

// An upload that did not reach the remote. Carries the local path because the
// edits are still there and still recoverable — that is the one thing the user
// needs to be told beyond the fact that the save did not land.
struct SaveFailure {
    term::session::SessionId session = 0;
    std::string              remotePath;
    std::string              localPath;
    std::string              message;
};

// Owns one active remote-edit round-trip:
//   download → editor launch → inotify watch → coalesced SFTP upload on each save.
//
// Thread-safety contract:
//   All public methods except Stop() must be called on the UI thread.
//   Stop() is safe to call from any thread.
//   Destructor calls Stop(); never destroy from a background thread while the
//   watch loop might be calling back into the manager.
class RemoteEditSession {
public:
    RemoteEditSession(term::session::SessionId     sessionId,
                      std::string                  remotePath,
                      std::string                  localPath,
                      term::session::SessionManager& sm);
    ~RemoteEditSession();

    // Non-copyable, non-movable: the dispatch guard is held by callbacks.
    RemoteEditSession(const RemoteEditSession&)            = delete;
    RemoteEditSession& operator=(const RemoteEditSession&) = delete;

    // Reports a save that has reached the remote: the session it landed on and
    // the absolute remote path. Invoked on the UI thread, once per successful
    // upload, so an observer can re-read what the write changed.
    using SavedFn = std::function<void(term::session::SessionId,
                                       const std::string& remotePath)>;
    void SetOnSaved(SavedFn cb) { onSaved_ = std::move(cb); }

    // Reports a save that did NOT reach the remote. Invoked on the UI thread.
    // A silent failure is the dangerous one: the editor reported a clean write,
    // so without this the user believes the remote file was updated.
    // Consecutive failures carrying the same message are reported once — a
    // dropped connection would otherwise raise one dialog per keystroke-save —
    // and a successful upload re-arms reporting.
    using FailedFn = std::function<void(const SaveFailure&)>;
    void SetOnSaveFailed(FailedFn cb) { onSaveFailed_ = std::move(cb); }

    // Starts the inotify watch loop. Must be called once after construction.
    void Start();

    // Stops the watch loop (write to stop-pipe, join thread, close fds), then
    // removes the working copy. Idempotent; safe from any thread.
    void Stop();

    term::session::SessionId GetSessionId()  const { return sessionId_;   }
    const std::string&       GetRemotePath() const { return remotePath_;  }
    const std::string&       GetLocalPath()  const { return localPath_;   }

private:
    void WatchLoop();
    void TriggerUpload();

    // Applies the announce policy to one finished upload and re-triggers a
    // coalesced save if one arrived while this was in flight. UI thread only.
    void OnUploadFinished(const term::transport::FsError& err);

    term::session::SessionId       sessionId_;
    std::string                    remotePath_;
    std::string                    localPath_;
    term::session::SessionManager& sm_;
    SavedFn                        onSaved_;
    FailedFn                       onSaveFailed_;

    // Last failure announced to the observer, "" once a save succeeds.
    // UI thread only: written and read solely in the upload continuation.
    std::string                    lastReportedError_;

    // Retires the watch and upload callbacks. Retired by Stop() rather than
    // only by destruction: a stopped session lingers until its owner erases it,
    // and an upload continuation must not fire in that window.
    term::fs::DispatchGuard guard_{
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); }};

    std::atomic<bool> uploadInFlight_{false};
    std::atomic<bool> pendingUpload_{false};

    int inotifyFd_    = -1;
    int stopPipe_[2]  = {-1, -1};
    std::thread watchThread_;
};

} // namespace ui
