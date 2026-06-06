#pragma once
#include "session/SessionManager.h"
#include "ui/RemoteEditSession.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

// Application-global manager for remote-edit sessions.
// Owned by App; all methods must be called on the UI thread.
class RemoteEditManager {
public:
    explicit RemoteEditManager(term::session::SessionManager& sm);
    ~RemoteEditManager();

    // Downloads remotePath for the given session, then launches editorCommand
    // with the local temp file path appended.  Calls onReady(true, "") on
    // success or onReady(false, errorMessage) on failure.
    // onReady is invoked on the UI thread.
    void OpenRemoteFile(term::session::SessionId  id,
                        const std::string&        remotePath,
                        const std::string&        editorCommand,
                        std::function<void(bool, std::string)> onReady);

    // Stops a single edit session identified by local temp path and removes it.
    void StopSession(const std::string& localPath);

    // Stops and removes all edit sessions belonging to the given SSH session.
    // Called when an SSH session is destroyed.
    void OnSessionDestroyed(term::session::SessionId id);

    const std::vector<std::unique_ptr<RemoteEditSession>>& GetSessions() const {
        return sessions_;
    }

private:
    term::session::SessionManager&                  sm_;
    std::shared_ptr<std::atomic<bool>>              alive_;
    std::vector<std::unique_ptr<RemoteEditSession>> sessions_;
};

} // namespace ui
