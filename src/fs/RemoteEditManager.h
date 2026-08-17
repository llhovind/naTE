#pragma once
#include "fs/Dispatcher.h"
#include "fs/EditEndpoint.h"
#include "fs/RemoteEditSession.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace term::fs {

// One edit currently in progress, flattened to what an observer needs to show
// it and to act on it. A snapshot: the sessions behind it are owned by the
// manager and may be gone by the time this is read, which is why the identity
// carried here is the local path rather than a pointer.
struct ActiveEdit {
    std::string remotePath;
    std::string localPath;
    // The endpoint's label, captured when the edit opened. Held rather than
    // looked up, because by the time this is read the endpoint may be gone —
    // and a row that cannot name its host is worse than a slightly stale name.
    std::string host;
};

// Starts the user's editor on one file, given the configured command and the
// path to open. Injected rather than called directly: spawning a process is the
// one part of an edit that belongs to the platform, and holding it at arm's
// length is what lets the rest be exercised without one.
//
// Returns as soon as the editor is started, and nothing downstream waits on it:
// an editor's exit means "the buffer was closed" for one command and "the file
// was handed to an already-running instance" for another, so an edit is driven
// by the file being written and never by the process finishing.
using LaunchEditorFn = std::function<void(const std::string& command,
                                          const std::string& path)>;

// Application-global manager for remote-edit sessions.
// Owned by App; all methods must be called on the owning thread.
class RemoteEditManager {
public:
    // dispatch marshals worker-thread callbacks onto the thread that owns this
    // manager, and is handed on to every session it creates.
    RemoteEditManager(Dispatcher     dispatch,
                      LaunchEditorFn launchEditor);
    ~RemoteEditManager();

    // Downloads remotePath from the given endpoint, then launches editorCommand
    // with the local temp file path appended.  Calls onReady(true, "") on
    // success or onReady(false, errorMessage) on failure.
    // onReady is invoked on the owning thread.
    void OpenRemoteFile(EditEndpoint       endpoint,
                        const std::string& remotePath,
                        const std::string& editorCommand,
                        std::function<void(bool, std::string)> onReady);

    // Reports every save that reaches a remote, as (endpoint, remote path), on
    // the owning thread. The manager does not know who cares — an open explorer
    // showing that directory is holding a listing the write just invalidated —
    // so the owner routes it.
    using SavedFn = RemoteEditSession::SavedFn;
    void SetOnFileSaved(SavedFn cb) { onFileSaved_ = std::move(cb); }

    // Reports a save that never reached the remote, on the owning thread.
    // Routed by the owner for the same reason as SetOnFileSaved: the manager
    // knows which endpoint failed, not which window should tell the user.
    using FailedFn = RemoteEditSession::FailedFn;
    void SetOnFileSaveFailed(FailedFn cb) { onFileSaveFailed_ = std::move(cb); }

    // Stops a single edit session identified by local temp path and removes it,
    // which discards its working copy. This is how an edit ends on purpose:
    // nothing else can tell, because the editor is launched detached and its
    // exit means different things for different editors.
    void StopSession(const std::string& localPath);

    // Stops and removes every edit whose file lives on the given filesystem.
    //
    // Keyed on the port rather than on a session id: that is the only identity
    // this layer holds, and it is the one that actually matters — once the
    // filesystem is gone there is nowhere left to upload to. Call this while
    // the pointer still resolves, i.e. as the session is being destroyed and
    // not after. A null argument matches nothing.
    void StopEditsForFilesystem(const transport::IRemoteFileSystem* fs);

    // Every edit in progress, newest last. Read live at the point of use rather
    // than cached by an observer — an edit can end for reasons the observer
    // never sees, such as its SSH session going away.
    std::vector<ActiveEdit> ListActiveEdits() const;

    // Whether ListActiveEdits would return anything. Separate because the menu
    // guard asks this on every idle cycle and only needs the answer, not the
    // vector of strings building one would allocate.
    bool HasActiveEdits() const { return !sessions_.empty(); }

private:
    // Kept as well as being held by guard_, which does not hand it back: every
    // session this manager creates needs the same one.
    Dispatcher                                      dispatch_;
    LaunchEditorFn                                  launchEditor_;
    SavedFn                                         onFileSaved_;
    FailedFn                                        onFileSaveFailed_;
    std::vector<std::unique_ptr<RemoteEditSession>> sessions_;
    // Declared last so it is destroyed first: the download continuation it
    // guards touches sessions_, which must still exist when the guard retires.
    DispatchGuard                                   guard_;
};

} // namespace term::fs
