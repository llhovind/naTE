#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "document/IDocumentListener.h"
#include "input/InputRouter.h"
#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "session/ISessionObserver.h"
#include "session/Session.h"

namespace term::session {

class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // -------------------------------------------------------------------------
    // Session lifecycle
    // -------------------------------------------------------------------------

    SessionId CreateSession(const Connection& conn,
                            int scrollbackLines,
                            unsigned short cols,
                            unsigned short rows,
                            AppSessionDefaults appDefaults = {},
                            unsigned short ptyLineWidth = 1024);

    // Stops the transport, fires OnSessionDestroyed on the per-session observer,
    // then destroys the session record.
    void CloseSession(SessionId id);
    void CloseAllSessions();

    // -------------------------------------------------------------------------
    // Per-session routing — called by App after CreateSession
    // -------------------------------------------------------------------------

    // Registers session with the given router and records the router for later
    // ReassignRouter / CloseSession calls.
    void RegisterRouter(SessionId id, term::input::InputRouter& router);

    // Moves the session from one window's router to another's.
    void ReassignRouter(SessionId id,
                        term::input::InputRouter& newRouter);

    // Sets the focused InputTarget in router to this session.
    void ActivateSession(SessionId id, term::input::InputRouter& router);

    // -------------------------------------------------------------------------
    // Per-session observer (disconnect / error / destroyed callbacks)
    // -------------------------------------------------------------------------

    // Atomically updates the per-session observer pointer. Safe to call from
    // the UI thread while the session thread may be firing callbacks.
    void SetSessionObserver(SessionId id, ISessionObserver* obs);

    // -------------------------------------------------------------------------
    // Per-session document listener (for UIManager's SessionNotifier)
    // -------------------------------------------------------------------------

    // Attach / detach a document listener directly on the session's active doc.
    // Both are safe to call with the transport thread running (Document mutex).
    void AttachSessionListener(SessionId id, IDocumentListener* l);
    void DetachSessionListener(SessionId id, IDocumentListener* l);

    // Returns a callable that reads the session title on whichever thread calls
    // it. Caller is responsible for ensuring the session outlives the returned
    // function (SessionManager guarantees this while the session is registered).
    std::function<std::string()> MakeTitleGetter(SessionId id) const;

    // Overload that supports a static profile title override. When useProfileTitle
    // is true and profileTitle is non-empty, the getter always returns profileTitle
    // regardless of any transport-sent OSC title changes.
    std::function<std::string()> MakeTitleGetter(SessionId id,
                                                  const std::string& profileTitle,
                                                  bool useProfileTitle) const;

    // -------------------------------------------------------------------------
    // Viewport control — called by UIManager on the UI thread
    // -------------------------------------------------------------------------

    DocLayout&  GetDocLayout(SessionId id) const;
    std::string GetLabel(SessionId id) const;

    void OnScroll(SessionId id, int topRow);
    void OnResize(SessionId id, unsigned short cols, unsigned short rows);
    void SetWrapMode(SessionId id, bool wrap);
    void ResetTerminal(SessionId id, bool clearScrollback);

    bool        SupportsFileTransfer(SessionId id) const;
    std::string GetRemoteDescription(SessionId id) const;
    void        TransferFile(SessionId id,
                             const std::string& localPath,
                             const std::string& remoteDir,
                             std::function<void(bool, std::string)> onDone);
    void        ReceiveFile(SessionId id,
                            const std::string& remotePath,
                            const std::string& localDir,
                            std::function<void(bool, std::string)> onDone);
    void        ListRemoteDirectory(
                    SessionId id,
                    const std::string& remotePath,
                    std::function<void(std::vector<transport::RemoteDirEntry>,
                                       std::string)> onDone);

    term::input::InputTarget* GetInputTarget(SessionId id) const;

private:
    struct SessionRecord {
        std::unique_ptr<Session>                         session;
        std::string                                      label;
        term::input::InputRouter*                        router     = nullptr;
        // Heap-allocated so lambdas captured in the Session constructor can
        // hold a stable pointer even if sessions_ is rehashed.
        std::shared_ptr<std::atomic<ISessionObserver*>>  uiObserver;
        // Profile-title override, retained so TakeSession works correctly after
        // a session is dragged to a new tile/window.
        std::string                                      profileTitle;
        bool                                             useProfileTitle = false;
    };

    SessionRecord*       FindRecord(SessionId id);
    const SessionRecord* FindRecord(SessionId id) const;

    // sessions_ owns all records via unique_ptr so pointers to members
    // (e.g. uiObserver) remain stable across map rehashes.
    std::unordered_map<SessionId, std::unique_ptr<SessionRecord>> sessions_;
    std::vector<SessionId>                                         pendingClose_;
    SessionId                                                      nextId_ = 1;
};

} // namespace term::session
