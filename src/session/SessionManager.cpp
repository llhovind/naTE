#include "session/SessionManager.h"
#include <algorithm>
#include <stdexcept>

namespace term::session {

// ---------------------------------------------------------------------------
// DocumentObserver  (runs on Session thread)
// ---------------------------------------------------------------------------

void SessionManager::DocumentObserver::OnDocumentChanged(DocChangeType type, size_t)
{
    if (type == DocChangeType::TitleChanged)
        manager->OnTitleChanged(id, getTitle());
    else
        manager->OnDocumentDirty(id);
}

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------

SessionManager::SessionManager(term::input::InputRouter& router)
    : router_(router)
{}

SessionManager::~SessionManager()
{
    // Stop transport threads first so no callbacks are in-flight when we
    // deregister observers and destroy session records.
    for (auto& [id, rec] : sessions_) {
        rec.session->Stop();
        rec.session->RemoveDocumentListener(rec.docObserver.get());
        router_.RemoveTarget(rec.session.get());
    }
}

void SessionManager::SetObserver(ISessionObserver* observer)
{
    observer_ = observer;
}

SessionId SessionManager::CreateSession(const Connection& conn,
                                        int scrollbackLines,
                                        unsigned short cols,
                                        unsigned short rows)
{
    const SessionId id = nextId_++;

    SessionRecord record;
    record.label = conn.label;

    record.session = std::make_unique<Session>(
        conn,
        scrollbackLines,
        cols,
        rows,
        [this, id]() {
            if (observer_)
                observer_->OnSessionDisconnected(id);
        });

    record.docObserver = std::make_unique<DocumentObserver>();
    record.docObserver->id       = id;
    record.docObserver->manager  = this;
    // Capture the session pointer; safe because Session outlives its observer.
    Session* sess = record.session.get();
    record.docObserver->getTitle = [sess]() { return sess->GetTitle(); };

    record.session->AddDocumentListener(record.docObserver.get());
    router_.AddTarget(record.session.get());
    sessions_.emplace(id, std::move(record));

    if (observer_)
        observer_->OnSessionCreated(id, conn.label);

    return id;
}

void SessionManager::ActivateSession(SessionId id)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec) return;

    router_.SetFocused(rec->session.get());
    activeId_ = id;
}

void SessionManager::CloseSession(SessionId id)
{
    // Guard against re-entrant calls (e.g. disconnect fires while already closing)
    // and against double-close of an already-removed session.
    for (SessionId pending : pendingClose_)
        if (pending == id) return;
    if (!FindRecord(id)) return;

    pendingClose_.push_back(id);

    // Stop the transport thread first so no DocumentObserver callbacks are
    // in-flight when we deregister and destroy the session record.
    if (SessionRecord* rec = FindRecord(id)) {
        rec->session->Stop();
        rec->session->RemoveDocumentListener(rec->docObserver.get());
        router_.RemoveTarget(rec->session.get());
        if (activeId_ == id)
            activeId_ = 0;
    }

    if (observer_)
        observer_->OnSessionDestroyed(id);

    sessions_.erase(id);

    pendingClose_.erase(
        std::remove(pendingClose_.begin(), pendingClose_.end(), id),
        pendingClose_.end());
}

void SessionManager::CloseAllSessions()
{
    // Snapshot IDs first — CloseSession erases from sessions_ while iterating.
    std::vector<SessionId> ids;
    ids.reserve(sessions_.size());
    for (auto& [id, _] : sessions_)
        ids.push_back(id);
    for (SessionId id : ids)
        CloseSession(id);
}

DocLayout& SessionManager::GetDocLayout(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    if (!rec)
        throw std::out_of_range("SessionManager::GetDocLayout: unknown SessionId");
    return rec->session->GetDocLayout();
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

// Session thread — must not access sessions_.
void SessionManager::OnDocumentDirty(SessionId id)
{
    if (observer_)
        observer_->OnSessionRefresh(id);
}

// Session thread — must not access sessions_.
void SessionManager::OnTitleChanged(SessionId id, const std::string& title)
{
    if (observer_)
        observer_->OnSessionTitleChanged(id, title);
}

SessionManager::SessionRecord* SessionManager::FindRecord(SessionId id)
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? &it->second : nullptr;
}

const SessionManager::SessionRecord* SessionManager::FindRecord(SessionId id) const
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? &it->second : nullptr;
}

} // namespace term::session
