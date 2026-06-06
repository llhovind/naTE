#pragma once
#include "session/SessionManager.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace ui {

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

    // Non-copyable, non-movable: shared_ptr to alive_ is held by callbacks.
    RemoteEditSession(const RemoteEditSession&)            = delete;
    RemoteEditSession& operator=(const RemoteEditSession&) = delete;

    // Starts the inotify watch loop. Must be called once after construction.
    void Start();

    // Stops the watch loop (write to stop-pipe, join thread, close fds).
    // Idempotent; safe from any thread.
    void Stop();

    // Builds the local temp path: /tmp/nate-edit/<hostname>/<encoded-path>/<filename>
    // Path components with '/' are encoded as '%2F'.
    static std::string MakeTempPath(const std::string& hostname,
                                    const std::string& remotePath);

    term::session::SessionId GetSessionId()  const { return sessionId_;   }
    const std::string&       GetRemotePath() const { return remotePath_;  }
    const std::string&       GetLocalPath()  const { return localPath_;   }

private:
    void WatchLoop();
    void TriggerUpload();

    term::session::SessionId       sessionId_;
    std::string                    remotePath_;
    std::string                    localPath_;
    term::session::SessionManager& sm_;

    // Shared with the upload callback so it can detect destruction safely.
    std::shared_ptr<std::atomic<bool>> alive_;

    std::atomic<bool> uploadInFlight_{false};
    std::atomic<bool> pendingUpload_{false};

    int inotifyFd_    = -1;
    int stopPipe_[2]  = {-1, -1};
    std::thread watchThread_;
};

} // namespace ui
