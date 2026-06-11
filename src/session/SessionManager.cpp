#include "session/SessionManager.h"
#include "db/IScrollbackRepository.h"
#include "db/ScrollbackWriter.h"
#include "transport/TransportError.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <cstdio>

namespace term::session {

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------

SessionManager::SessionManager() = default;

SessionManager::SessionManager(
    std::unique_ptr<term::db::IScrollbackRepository> scrollbackRepo,
    std::string scrollbackDir,
    size_t scrollbackSaveLines,
    bool   scrollbackSaveStyles)
    : scrollbackRepo_(std::move(scrollbackRepo))
    , scrollbackDir_(std::move(scrollbackDir))
    , scrollbackSaveLines_(scrollbackSaveLines)
    , scrollbackSaveStyles_(scrollbackSaveStyles)
{}

SessionManager::~SessionManager()
{
    // Compact scrollback before stopping transports so segment files are merged.
    if (scrollbackRepo_)
        CompactAllSessions();

    // Stop all transport threads before records are destroyed. Listeners were
    // already detached by UIManager::~UIManager (which is destroyed before
    // SessionManager in WindowContext order) or by explicit CloseSession calls.
    for (auto& [id, rec] : sessions_) {
        rec->session->Stop();
        if (rec->router)
            rec->router->RemoveTarget(rec->session.get());
    }
}

// static
std::string SessionManager::GenerateUuid()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(gen);
    uint64_t lo = dist(gen);

    // Set version 4
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // Set RFC 4122 variant (10xx)
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<uint32_t>(hi >> 32),
        static_cast<uint16_t>(hi >> 16),
        static_cast<uint16_t>(hi & 0xFFFF),
        static_cast<uint16_t>(lo >> 48),
        static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFULL));
    return buf;
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

SessionId SessionManager::CreateSession(const Connection& conn,
                                        int scrollbackLines,
                                        unsigned short cols,
                                        unsigned short rows,
                                        AppSessionDefaults appDefaults,
                                        unsigned short ptyLineWidth,
                                        std::string uuid)
{
    const SessionId id = nextId_++;

    auto rec = std::make_unique<SessionRecord>();
    rec->label          = conn.label;
    rec->profileTitle   = conn.profileTitle;
    rec->useProfileTitle= conn.useProfileTitle;
    rec->connection     = conn;
    rec->appDefaults    = appDefaults;
    rec->uiObserver     = std::make_shared<std::atomic<ISessionObserver*>>(nullptr);
    rec->scrollbackUuid = uuid.empty() ? GenerateUuid() : std::move(uuid);

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

    // Create and start a scrollback writer if the feature is enabled.
    if (scrollbackRepo_) {
        auto writer = std::make_unique<term::db::ScrollbackWriter>(
            rec->session->GetMainDoc(),
            scrollbackDir_,
            rec->scrollbackUuid,
            scrollbackSaveLines_,
            scrollbackSaveStyles_);
        writer->Start();
        rec->session->AddDocumentListener(writer.get());
        // Wire the clear-scrollback callback (UI thread → writer).
        rec->session->onClearScrollback_ = [w = writer.get()]{ w->Truncate(); };
        rec->scrollbackWriter = std::move(writer);
    }

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
        // Compact scrollback before the writer is destroyed with the record.
        if (rec->scrollbackWriter)
            rec->scrollbackWriter->Compact();

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
    if (SessionRecord* rec = FindRecord(id)) {
        rec->session->SetViewportSize(cols, rows);
        rec->connection.columnWidth = cols;
        rec->connection.rows        = rows;
    }
}

void SessionManager::SetWrapMode(SessionId id, bool wrap)
{
    if (SessionRecord* rec = FindRecord(id)) {
        rec->session->SetWrapMode(wrap);
        rec->connection.wrapMode = wrap;
    }
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

bool SessionManager::SupportsPortForwarding(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec && rec->session->SupportsPortForwarding();
}

transport::PortForwardId SessionManager::AddPortForward(SessionId id,
                                                           transport::PortForwardDesc desc)
{
    if (SessionRecord* rec = FindRecord(id))
        return rec->session->AddPortForward(std::move(desc));
    return 0;
}

void SessionManager::RemovePortForward(SessionId id, transport::PortForwardId fwdId)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->RemovePortForward(fwdId);
}

void SessionManager::SetPortForwardChangedCallback(
    SessionId id,
    std::function<void(std::vector<transport::PortForwardStatus>)> cb)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->SetPortForwardChangedCallback(std::move(cb));
}

std::vector<transport::PortForwardStatus> SessionManager::GetPortForwardStatus(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec ? rec->session->GetPortForwardStatus() : std::vector<transport::PortForwardStatus>{};
}

std::vector<transport::PortForwardDesc> SessionManager::GetPortForwardDescs(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec ? rec->session->GetPortForwardDescs() : std::vector<transport::PortForwardDesc>{};
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

void SessionManager::TransferFileBetweenSessions(
    SessionId          srcId,
    const std::string& srcPath,
    SessionId          dstId,
    const std::string& dstDir,
    std::function<void(bool, std::string)> onDone)
{
    // Local → Remote
    if (srcId == 0) {
        SendFile(dstId, srcPath, dstDir, std::move(onDone));
        return;
    }
    // Remote → Local
    if (dstId == 0) {
        ReceiveFile(srcId, srcPath, dstDir, std::move(onDone));
        return;
    }
    // Remote → Remote: download to temp, upload, then clean up.
    std::filesystem::path tempDir;
    try {
        tempDir = std::filesystem::temp_directory_path() / "nate_xfer_XXXXXXXX";
        // Replace the X's with a unique suffix.
        tempDir = std::filesystem::path(
            std::string(tempDir) + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(tempDir);
    } catch (const std::exception& ex) {
        onDone(false, std::string("Failed to create temp directory: ") + ex.what());
        return;
    }

    const std::string tempDirStr = tempDir.string();

    ReceiveFile(srcId, srcPath, tempDirStr,
        [this, dstId, dstDir, tempDirStr, srcPath,
         onDone = std::move(onDone)](bool ok, std::string err) mutable {

            if (!ok) {
                std::filesystem::remove_all(tempDirStr);
                onDone(false, std::move(err));
                return;
            }

            const std::string filename =
                std::filesystem::path(srcPath).filename().string();
            const std::string tempFile =
                (std::filesystem::path(tempDirStr) / filename).string();

            SendFile(dstId, tempFile, dstDir,
                [tempDirStr, onDone = std::move(onDone)](bool ok2, std::string err2) {
                    std::filesystem::remove_all(tempDirStr);
                    onDone(ok2, std::move(err2));
                });
        });
}

void SessionManager::ListRemoteDirectory(
    SessionId id,
    const std::string& remotePath,
    std::function<void(std::vector<transport::RemoteDirEntry>, std::string)> onDone)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->ListRemoteDirectory(remotePath, std::move(onDone));
}

void SessionManager::SftpDownloadFile(SessionId id,
                                      const std::string& remotePath,
                                      const std::string& localPath,
                                      std::function<void(bool, std::string)> onDone)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->SftpDownloadFile(remotePath, localPath, std::move(onDone));
}

void SessionManager::SftpUploadFile(SessionId id,
                                    const std::string& localPath,
                                    const std::string& remotePath,
                                    std::function<void(bool, std::string)> onDone)
{
    SessionRecord* rec = FindRecord(id);
    if (rec) rec->session->SftpUploadFile(localPath, remotePath, std::move(onDone));
}

// ---------------------------------------------------------------------------
// Scrollback persistence
// ---------------------------------------------------------------------------

std::string SessionManager::GetScrollbackUuid(SessionId id) const
{
    const SessionRecord* rec = FindRecord(id);
    return rec ? rec->scrollbackUuid : std::string{};
}

void SessionManager::LoadScrollback(SessionId id, const ScrollbackSnapshot& snap)
{
    if (SessionRecord* rec = FindRecord(id))
        rec->session->LoadScrollback(snap);
}

void SessionManager::StartScrollbackWriter(SessionId id,
                                           const ScrollbackSnapshot* initial)
{
    SessionRecord* rec = FindRecord(id);
    if (!rec || !rec->scrollbackWriter) return;
    rec->scrollbackWriter->Start(initial);
}

void SessionManager::CompactScrollback(SessionId id)
{
    SessionRecord* rec = FindRecord(id);
    if (rec && rec->scrollbackWriter)
        rec->scrollbackWriter->Compact();
}

void SessionManager::CompactAllSessions()
{
    for (auto& [id, rec] : sessions_)
        if (rec->scrollbackWriter)
            rec->scrollbackWriter->Compact();
}

void SessionManager::PurgeScrollbackForState(const RestoreState& state)
{
    if (!scrollbackRepo_) return;
    for (const auto& window : state.windows)
        for (const auto& tile : window.tiles)
            for (const auto& session : tile.sessions)
                if (!session.scrollbackUuid.empty())
                    scrollbackRepo_->Delete(session.scrollbackUuid);
}

void SessionManager::RestoreScrollback(SessionId id, const std::string& uuid)
{
    if (!scrollbackRepo_ || uuid.empty()) return;
    if (!scrollbackRepo_->Exists(uuid)) return;

    auto snap = scrollbackRepo_->Load(uuid, scrollbackSaveLines_);
    if (snap.lines.empty()) return;

    const auto now   = std::chrono::system_clock::now();
    const auto ttime = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&ttime), "%Y-%m-%d %H:%M:%S");
    snap.savedAt = ss.str();

    SessionRecord* rec = FindRecord(id);
    if (!rec) return;

    rec->session->LoadScrollback(snap);

    // Append the separator DocLine to the snapshot so it is saved to disk and
    // appears in subsequent restores, marking each restore boundary.
    {
        const std::u32string t(snap.savedAt.begin(), snap.savedAt.end());
        const std::u32string text = U"--- Scrollback restored from " + t + U" ---";
        DocLine sep;
        sep.text = text;
        sep.styles.push_back({0, text.size(), Style{.fg = 8, .dim = true}});
        snap.lines.push_back(std::move(sep));
    }

    // Re-start the writer pre-populated with the restored snapshot so segments
    // are the single source of truth for crash recovery (Fix 1).
    if (rec->scrollbackWriter)
        rec->scrollbackWriter->Start(&snap);
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
