#include "session/SessionManager.h"
#include "transport/TransportError.h"
#include <algorithm>
#include <stdexcept>

namespace term::session {

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------

SessionManager::SessionManager() = default;

SessionManager::~SessionManager()
{
    // Stop all transport threads before records are destroyed. Listeners were
    // already detached by UIManager::~UIManager (which is destroyed before
    // SessionManager in WindowContext order) or by explicit CloseSession calls.
    for (auto& [id, rec] : sessions_) {
        rec->session->Stop();
        if (rec->router)
            rec->router->RemoveTarget(rec->session.get());
    }
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

SessionId SessionManager::CreateSession(const Connection& conn,
                                        int scrollbackLines,
                                        unsigned short cols,
                                        unsigned short rows,
                                        AppSessionDefaults appDefaults,
                                        unsigned short ptyLineWidth)
{
    const SessionId id = nextId_++;

    auto rec = std::make_unique<SessionRecord>();
    rec->label          = conn.label;
    rec->profileTitle   = conn.profileTitle;
    rec->useProfileTitle= conn.useProfileTitle;
    rec->connection     = conn;
    rec->appDefaults    = appDefaults;
    rec->uiObserver     = std::make_shared<std::atomic<ISessionObserver*>>(nullptr);

    // Capture the shared_ptr so the lambda remains valid even if this record
    // is erased from the map before the session thread fires the callback.
    auto uiObs = rec->uiObserver;

    rec->session = std::make_unique<Session>(
        conn,
        scrollbackLines,
        cols,
        rows,
        [uiObs, id](transport::DisconnectReason reason) {
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                obs->OnSessionDisconnected(id, reason);
        },
        [uiObs, id](const transport::TransportError& err) {
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                obs->OnSessionError(id, err);
        },
        std::move(appDefaults),
        ptyLineWidth,
        conn.wrapMode,
        [uiObs, id](bool active) {
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                obs->OnAltScreenChanged(id, active);
        },
        // Capture a raw pointer to the record: it is heap-allocated via unique_ptr
        // so its address is stable even after sessions_.emplace moves the unique_ptr.
        [uiObs, id, recPtr = rec.get()](bool active) {
            recPtr->x11ForwardingActive.store(active, std::memory_order_release);
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                obs->OnX11FwdChanged(id, active);
        },
        [uiObs, id](const transport::KbdIntChallenge& challenge)
            -> std::vector<std::string> {
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                return obs->OnKbdIntChallenge(id, challenge);
            return {};
        },
        [uiObs, id]() {
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                obs->OnBell(id);
        },
        [uiObs, id](bool visible) {
            if (auto* obs = uiObs->load(std::memory_order_acquire))
                obs->OnCursorVisibilityChanged(id, visible);
        });

    sessions_.emplace(id, std::move(rec));
    return id;
}

void SessionManager::CloseSession(SessionId id)
{
    for (SessionId pending : pendingClose_)
        if (pending == id) return;
    if (!FindRecord(id)) return;

    pendingClose_.push_back(id);

    if (SessionRecord* rec = FindRecord(id)) {
        rec->session->Stop();

        if (rec->router)
            rec->router->RemoveTarget(rec->session.get());

        // Notify the current UIManager to tear down its tile and detach its
        // SessionNotifier. OnSessionDestroyed is always called on the UI thread
        // (CloseSession is UI-thread-only), so the notifier detach is safe.
        if (auto* obs = rec->uiObserver->load(std::memory_order_acquire))
            obs->OnSessionDestroyed(id);
    }

    sessions_.erase(id);

    pendingClose_.erase(
        std::remove(pendingClose_.begin(), pendingClose_.end(), id),
        pendingClose_.end());
}

void SessionManager::CloseAllSessions()
{
    std::vector<SessionId> ids;
    ids.reserve(sessions_.size());
    for (auto& [id, _] : sessions_)
        ids.push_back(id);
    for (SessionId id : ids)
        CloseSession(id);
}

void SessionManager::ReconnectSession(SessionId id)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    rec->session->ReplaceConnection(rec->connection, rec->appDefaults);
}

// ---------------------------------------------------------------------------
// Per-session routing
// ---------------------------------------------------------------------------

void SessionManager::RegisterRouter(SessionId id, term::input::InputRouter& router)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    rec->router = &router;
    router.AddTarget(rec->session.get());
}

void SessionManager::ReassignRouter(SessionId id,
                                    term::input::InputRouter& newRouter)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    if (rec->router) rec->router->RemoveTarget(rec->session.get());
    rec->router = &newRouter;
    newRouter.AddTarget(rec->session.get());
}

void SessionManager::ActivateSession(SessionId id, term::input::InputRouter& router)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    router.SetFocused(rec->session.get());
}

// ---------------------------------------------------------------------------
// Per-session observer
// ---------------------------------------------------------------------------

void SessionManager::SetSessionObserver(SessionId id, ISessionObserver* obs)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    rec->uiObserver->store(obs, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Per-session document listener
// ---------------------------------------------------------------------------

void SessionManager::AttachSessionListener(SessionId id, IDocumentListener* l)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    rec->session->AddDocumentListener(l);
}

void SessionManager::DetachSessionListener(SessionId id, IDocumentListener* l)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;
    rec->session->RemoveDocumentListener(l);
}

std::function<std::string()> SessionManager::MakeTitleGetter(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    if (!rec) return []{ return std::string{}; };
    return MakeTitleGetter(id, rec->profileTitle, rec->useProfileTitle);
}

std::function<std::string()> SessionManager::MakeTitleGetter(SessionId id,
                                                               const std::string& profileTitle,
                                                               bool useProfileTitle) const
{
    const SessionRecord* rec = FindRecord(id);
    if (!rec) return []{ return std::string{}; };

    if (useProfileTitle && !profileTitle.empty())
        return [profileTitle]{ return profileTitle; };

    Session* sess = rec->session.get();
    return [sess]{ return sess->GetTitle(); };
}

// ---------------------------------------------------------------------------
// Viewport control
// ---------------------------------------------------------------------------

DocLayout& SessionManager::GetDocLayout(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    if (!rec)
        throw std::out_of_range("SessionManager::GetDocLayout: unknown SessionId");
    return rec->session->GetDocLayout();
}

std::string SessionManager::GetLabel(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec ? rec->label : std::string{};
}

void SessionManager::OnScroll(SessionId id, int topRow)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->SetTopRow(topRow);
}

void SessionManager::OnResize(SessionId id, unsigned short cols, unsigned short rows)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->SetViewportSize(cols, rows);
}

void SessionManager::SetWrapMode(SessionId id, bool wrap)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->SetWrapMode(wrap);
}

term::input::InputTarget* SessionManager::GetInputTarget(SessionId id) const
{
    if (const SessionRecord* rec = FindRecord(id))
        return rec->session.get();
    return nullptr;
}

void SessionManager::ResetTerminal(SessionId id, bool clearScrollback)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->ResetTerminal(clearScrollback);
}

bool SessionManager::IsAltScreenActive(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec && rec->session->IsAltScreenActive();
}

bool SessionManager::IsBracketedPasteActive(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec && rec->session->IsBracketedPasteActive();
}

void SessionManager::ForceAltScreen(SessionId id, bool on)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->ForceAltScreen(on);
}

void SessionManager::RequestX11Forwarding(SessionId id)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->RequestX11Forwarding();
}

bool SessionManager::SupportsFileTransfer(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec && rec->session->SupportsFileTransfer();
}

bool SessionManager::SupportsX11Forwarding(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec && rec->session->SupportsX11Forwarding();
}

bool SessionManager::IsX11ForwardingActive(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec && rec->x11ForwardingActive.load(std::memory_order_acquire);
}

std::string SessionManager::GetRemoteDescription(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec ? rec->session->GetTransportRemoteDescription() : std::string{};
}

void SessionManager::SendFile(SessionId id,
                              const std::string& localPath,
                              const std::string& remoteDir,
                              std::function<void(bool, std::string)> onDone)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->SendFile(localPath, remoteDir, std::move(onDone));
}

void SessionManager::ReceiveFile(SessionId id,
                                 const std::string& remotePath,
                                 const std::string& localDir,
                                 std::function<void(bool, std::string)> onDone)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->ReceiveFile(remotePath, localDir, std::move(onDone));
}

void SessionManager::ListRemoteDirectory(
    SessionId id,
    const std::string& remotePath,
    std::function<void(std::vector<transport::RemoteDirEntry>, std::string)> onDone)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->ListRemoteDirectory(remotePath, std::move(onDone));
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

SessionManager::SessionRecord* SessionManager::FindRecord(SessionId id)
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

const SessionManager::SessionRecord* SessionManager::FindRecord(SessionId id) const
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

Connection SessionManager::GetConnection(SessionId id) const
{
    if (const SessionRecord* rec = FindRecord(id))
        return rec->connection;
    return {};
}

std::string SessionManager::GetCurrentWorkingDir(SessionId id) const
{
    if (const SessionRecord* rec = FindRecord(id))
        return rec->session->GetCurrentWorkingDir();
    return {};
}

} // namespace term::session
