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
#include "session/RestoreState.h"
#include "session/Session.h"

namespace term::db {
    class IScrollbackRepository;
    class ScrollbackWriter;
}

namespace term::session {

class SessionManager {
public:
    // Scrollback feature disabled.
    SessionManager();

    // scrollbackRepo may be null (feature disabled); remaining params are ignored when null.
    explicit SessionManager(
        std::unique_ptr<term::db::IScrollbackRepository> scrollbackRepo,
        std::string scrollbackDir = {},
        size_t scrollbackSaveLines = 0,
        bool   scrollbackSaveStyles = false);

    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // -------------------------------------------------------------------------
    // Session lifecycle
    // -------------------------------------------------------------------------

    // uuid: if non-empty, the session reuses this UUID for its scrollback file
    //       (restore path); if empty, a fresh UUID v4 is generated.
    SessionId CreateSession(const Connection& conn,
                            int scrollbackLines,
                            unsigned short cols,
                            unsigned short rows,
                            AppSessionDefaults appDefaults = {},
                            unsigned short ptyLineWidth = 1024,
                            std::string uuid = {});

    static std::string GenerateUuid();

    // Stops the transport, fires OnSessionDestroyed on the per-session observer,
    // then destroys the session record.
    void CloseSession(SessionId id);
    void CloseAllSessions();

    // Replaces the dead transport with a fresh one built from the stored Connection.
    // The session's Document, DocLayout, and all UI state are preserved.
    // Must be called on the UI thread after OnSessionDisconnected(Interrupted).
    void ReconnectSession(SessionId id);

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
    bool IsAltScreenActive(SessionId id)    const;
    bool IsBracketedPasteActive(SessionId id) const;
    void ForceAltScreen(SessionId id, bool on);
    void RequestX11Forwarding(SessionId id);

    SessionStatus GetSessionStatus(SessionId id) const;

    bool        SupportsFileTransfer(SessionId id)    const;
    bool        SupportsX11Forwarding(SessionId id)   const;
    bool        IsX11ForwardingActive(SessionId id)   const;
    bool        SupportsPortForwarding(SessionId id)  const;

    transport::PortForwardId AddPortForward(SessionId id, transport::PortForwardDesc desc);
    void        RemovePortForward(SessionId id, transport::PortForwardId fwdId);
    // Register the callback that UIManager hooks to update the port-forward panel.
    void        SetPortForwardChangedCallback(
                    SessionId id,
                    std::function<void(std::vector<transport::PortForwardStatus>)> cb);
    std::vector<transport::PortForwardStatus> GetPortForwardStatus(SessionId id) const;
    std::vector<transport::PortForwardDesc>   GetPortForwardDescs(SessionId id)  const;
    std::string GetRemoteDescription(SessionId id) const;
    void        SendFile(SessionId id,
                        const std::string& localPath,
                        const std::string& remoteDir,
                        std::function<void(bool, std::string)> onDone);
    void        ReceiveFile(SessionId id,
                            const std::string& remotePath,
                            const std::string& localDir,
                            std::function<void(bool, std::string)> onDone);

    // Unified transfer between any two endpoints. Pass SessionId 0 for the
    // local filesystem. Routes to SendFile / ReceiveFile for local↔remote
    // cases; uses a temp file for remote↔remote.
    void        TransferFileBetweenSessions(
                    SessionId          srcId,
                    const std::string& srcPath,
                    SessionId          dstId,
                    const std::string& dstDir,
                    std::function<void(bool, std::string)> onDone);
    void        ListRemoteDirectory(
                    SessionId id,
                    const std::string& remotePath,
                    std::function<void(std::vector<transport::RemoteDirEntry>,
                                       std::string)> onDone);
    void        SftpDownloadFile(SessionId id,
                                 const std::string& remotePath,
                                 const std::string& localPath,
                                 std::function<void(bool, std::string)> onDone);
    void        SftpUploadFile(SessionId id,
                               const std::string& localPath,
                               const std::string& remotePath,
                               std::function<void(bool, std::string)> onDone);

    term::input::InputTarget* GetInputTarget(SessionId id) const;

    // Returns a snapshot of the Connection used to create this session.
    // Returns a default-constructed Connection if the id is unknown.
    Connection GetConnection(SessionId id) const;

    // Returns the current working directory of the session's transport process.
    // Non-empty only for PTY sessions on Linux (/proc/<pid>/cwd).
    std::string GetCurrentWorkingDir(SessionId id) const;

    // -------------------------------------------------------------------------
    // Scrollback persistence
    // -------------------------------------------------------------------------

    // UUID for the session's scrollback file (empty if no repo is configured).
    std::string GetScrollbackUuid(SessionId id) const;

    // Inject a snapshot into the session's main document.
    void LoadScrollback(SessionId id, const ScrollbackSnapshot& snap);

    // Start the scrollback writer for a session. If initial is non-null,
    // pre-populates segment 0 with the snapshot before streaming begins (Fix 1).
    void StartScrollbackWriter(SessionId id,
                               const ScrollbackSnapshot* initial = nullptr);

    // Compact the session's scrollback writer (no-op if no writer).
    void CompactScrollback(SessionId id);

    // Compact all live sessions. Called on clean exit and explicit workspace save.
    void CompactAllSessions();

    // Delete scrollback files for every UUID referenced in state.
    // Call before deleting any restore/workspace file.
    void PurgeScrollbackForState(const RestoreState& state);

    // Load scrollback snapshot for uuid from repo, inject into session, and
    // start the writer pre-populated with the snapshot. No-op if uuid is empty
    // or no repo is configured.
    void RestoreScrollback(SessionId id, const std::string& uuid);

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
        // Snapshot of the Connection and app defaults used to create this session;
        // retained so CloseSession / ReconnectSession can recreate the transport.
        Connection                                       connection;
        AppSessionDefaults                               appDefaults;
        // Written by the worker thread (via onX11FwdChanged lambda); read by the
        // UI thread (IsX11ForwardingActive). Must be atomic — SessionRecord is
        // heap-allocated so its address is stable across map rehash.
        std::atomic<bool>                                x11ForwardingActive{false};
        // Scrollback persistence (null when feature is disabled).
        std::string                                      scrollbackUuid;
        std::unique_ptr<term::db::ScrollbackWriter>      scrollbackWriter;
    };

    SessionRecord*       FindRecord(SessionId id);
    const SessionRecord* FindRecord(SessionId id) const;

    // sessions_ owns all records via unique_ptr so pointers to members
    // (e.g. uiObserver) remain stable across map rehashes.
    std::unordered_map<SessionId, std::unique_ptr<SessionRecord>> sessions_;
    std::vector<SessionId>                                         pendingClose_;
    SessionId                                                      nextId_ = 1;

    std::unique_ptr<term::db::IScrollbackRepository> scrollbackRepo_;
    std::string                                       scrollbackDir_;
    size_t                                            scrollbackSaveLines_  = 0;
    bool                                              scrollbackSaveStyles_ = false;
};

} // namespace term::session
