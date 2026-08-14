#pragma once
#include "fs/Dispatcher.h"
#include "session/SessionManager.h"
#include "ui/RemoteEditSession.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/app.h>

namespace ui {

// One edit currently in progress, flattened to what an observer needs to show
// it and to act on it. A snapshot: the sessions behind it are owned by the
// manager and may be gone by the time this is read, which is why the identity
// carried here is the local path rather than a pointer.
struct ActiveEdit {
    term::session::SessionId session = 0;
    std::string              remotePath;
    std::string              localPath;
    // Resolved here rather than by the reader: it is the one field that needs a
    // SessionManager, and asking every observer to hold one just to render a
    // label would spread that dependency for nothing.
    std::string              host;
};

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

    // Reports every save that reaches a remote, as (session, remote path), on
    // the UI thread. The manager does not know who cares — an open explorer
    // showing that directory is holding a listing the write just invalidated —
    // so the owner routes it.
    using SavedFn = RemoteEditSession::SavedFn;
    void SetOnFileSaved(SavedFn cb) { onFileSaved_ = std::move(cb); }

    // Reports a save that never reached the remote, on the UI thread. Routed by
    // the owner for the same reason as SetOnFileSaved: the manager knows which
    // session failed, not which window should tell the user about it.
    using FailedFn = RemoteEditSession::FailedFn;
    void SetOnFileSaveFailed(FailedFn cb) { onFileSaveFailed_ = std::move(cb); }

    // Stops a single edit session identified by local temp path and removes it,
    // which discards its working copy. This is how an edit ends on purpose:
    // nothing else can tell, because the editor is launched detached and its
    // exit means different things for different editors.
    void StopSession(const std::string& localPath);

    // Stops and removes all edit sessions belonging to the given SSH session.
    // Called when an SSH session is destroyed.
    void OnSessionDestroyed(term::session::SessionId id);

    // Every edit in progress, newest last. Read live at the point of use rather
    // than cached by an observer — an edit can end for reasons the observer
    // never sees, such as its SSH session going away.
    std::vector<ActiveEdit> ListActiveEdits() const;

    // Whether ListActiveEdits would return anything. Separate because the menu
    // guard asks this on every idle cycle and only needs the answer, not the
    // vector of strings building one would allocate.
    bool HasActiveEdits() const { return !sessions_.empty(); }

private:
    term::session::SessionManager&                  sm_;
    SavedFn                                         onFileSaved_;
    FailedFn                                        onFileSaveFailed_;
    std::vector<std::unique_ptr<RemoteEditSession>> sessions_;
    // Declared last so it is destroyed first: the download continuation it
    // guards touches sessions_, which must still exist when the guard retires.
    term::fs::DispatchGuard                         guard_{
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); }};
};

} // namespace ui
