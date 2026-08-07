#pragma once

#include "config/Config.h"
#include "session/SessionManager.h"
#include "ui/FileExplorerFrame.h"

#include <functional>
#include <string>
#include <unordered_map>

class wxWindow;

namespace ui {

// Application-global owner of the per-session file explorer windows.
//
// App-global rather than per-window because a session can be dragged between
// windows without being destroyed (UIManager::ReleaseSession), which would
// leave a per-window map holding a session it no longer shows. Keying on
// SessionId and tearing down on OnSessionDestroyed is the only arrangement
// that survives a move.
//
// All methods must be called on the UI thread.
class FileExplorerManager {
public:
    // onOpenInEditor is invoked with (session, absolute remote path) when a
    // user asks to edit a file; the caller routes it to the remote-edit
    // workflow, which this class deliberately knows nothing about.
    FileExplorerManager(
        term::session::SessionManager& sm,
        const AppConfig& cfg,
        std::function<void(term::session::SessionId, std::string)> onOpenInEditor);

    ~FileExplorerManager();

    FileExplorerManager(const FileExplorerManager&)            = delete;
    FileExplorerManager& operator=(const FileExplorerManager&) = delete;

    // Opens the explorer for a session, or raises the existing window when one
    // is already open — a second window onto the same directory tree would
    // only fight with the first over the shared connection.
    void OpenForSession(wxWindow* parent, term::session::SessionId id);

    // Disables the window for a session that has gone away. The window is left
    // on screen rather than destroyed, so it does not vanish mid-interaction.
    void OnSessionDestroyed(term::session::SessionId id);

    void UpdateConfig(const AppConfig& cfg);

private:
    term::session::SessionManager& sm_;
    AppConfig                      cfg_;
    std::function<void(term::session::SessionId, std::string)> onOpenInEditor_;

    // Non-owning: wx owns its frames. Entries are removed when a frame reports
    // that it closed.
    std::unordered_map<term::session::SessionId, FileExplorerFrame*> frames_;
};

} // namespace ui
