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
    // onOpenInEditor is invoked with (endpoint, absolute path) when a user asks
    // to edit a file, where the endpoint is the session the file lives on or 0
    // for this computer; the caller routes it to the edit workflow, which this
    // class deliberately knows nothing about.
    // onGeometryChanged is invoked when a window's remembered shape changes,
    // so the owner can fold it into AppConfig and save. Kept as a callback
    // because writing configuration belongs to App, not to a UI manager.
    FileExplorerManager(
        term::session::SessionManager& sm,
        const AppConfig& cfg,
        std::function<void(term::session::SessionId, std::string)> onOpenInEditor,
        std::function<void(int width, int height)> onGeometryChanged = {});

    ~FileExplorerManager();

    FileExplorerManager(const FileExplorerManager&)            = delete;
    FileExplorerManager& operator=(const FileExplorerManager&) = delete;

    // Opens the explorer for a session in the requested mode, or switches and
    // raises the existing window when one is already open — a second window
    // onto the same tree would only fight with the first over the connection.
    // The menu item the user clicked names the mode, so it always wins.
    void OpenForSession(wxWindow* parent, term::session::SessionId id,
                        FileExplorerMode mode);

    // Tells every window that a session has gone away. Windows are left on
    // screen rather than destroyed — a pane pointed elsewhere is still useful,
    // and one that vanished mid-interaction would not be.
    void OnSessionDestroyed(term::session::SessionId id);

    void UpdateConfig(const AppConfig& cfg);

private:
    term::session::SessionManager& sm_;
    AppConfig                      cfg_;
    std::function<void(term::session::SessionId, std::string)> onOpenInEditor_;
    std::function<void(int, int)> onGeometryChanged_;

    // Non-owning: wx owns its frames. Entries are removed when a frame reports
    // that it closed.
    std::unordered_map<term::session::SessionId, FileExplorerFrame*> frames_;
};

} // namespace ui
