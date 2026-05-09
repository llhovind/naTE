#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "document/IDocumentListener.h"
#include "input/InputRouter.h"
#include "session/Connection.h"
#include "session/ISessionObserver.h"
#include "session/Session.h"

namespace term::session {

class SessionManager {
public:
    explicit SessionManager(term::input::InputRouter& router);
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    void SetObserver(ISessionObserver* observer);

    SessionId CreateSession(const Connection& conn,
                            int scrollbackLines,
                            unsigned short cols,
                            unsigned short rows,
                            unsigned short ptyLineWidth = 1024);

    void ActivateSession(SessionId id);
    void CloseSession(SessionId id);
    void CloseAllSessions();

    DocLayout&  GetDocLayout(SessionId id) const;
    SessionId   GetActiveSessionId() const { return activeId_; }

    void OnScroll(SessionId id, int topRow);
    void OnResize(SessionId id, unsigned short cols, unsigned short rows);
    void SetWordWrap(SessionId id, bool wrap);

private:
    // Listens to a Session's main Document and routes change events back to
    // SessionManager by SessionId — avoids any callback storage on Session.
    //
    // Runs entirely on the Session thread. getTitle() reads the Document
    // directly (same thread that just wrote the title) so no lock is needed.
    // Neither OnDocumentDirty nor OnTitleChanged access sessions_.
    struct DocumentObserver : IDocumentListener {
        SessionId                    id;
        SessionManager*              manager;
        std::function<std::string()> getTitle;
        void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;
    };

    struct SessionRecord {
        std::unique_ptr<Session>          session;
        std::unique_ptr<DocumentObserver> docObserver;
        std::string                       label;
    };

    // Both called on the Session thread — must not access sessions_.
    void OnDocumentDirty(SessionId id);
    void OnTitleChanged(SessionId id, const std::string& title);

    SessionRecord*       FindRecord(SessionId id);
    const SessionRecord* FindRecord(SessionId id) const;

    term::input::InputRouter& router_;
    ISessionObserver*         observer_ = nullptr;

    std::unordered_map<SessionId, SessionRecord> sessions_;
    std::vector<SessionId>                       pendingClose_;
    SessionId                                    nextId_   = 1;
    SessionId                                    activeId_ = 0;
};

} // namespace term::session
