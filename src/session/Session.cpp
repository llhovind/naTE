#include "session/Session.h"
#include "transport/PtyTransport.h"
#include "transport/LoopbackTransport.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace term::session {

// static
std::unique_ptr<transport::Transport> Session::MakeTransport(
    transport::ITransportTarget& target,
    const Connection& conn,
    unsigned short ptyCols,
    unsigned short rows,
    unsigned short viewportCols,
    const AppSessionDefaults& appDefaults)
{
    return std::visit([&](auto&& desc) -> std::unique_ptr<transport::Transport> {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, PtyDesc>)
            return std::make_unique<transport::PtyTransport>(
                target, desc.shell, desc.command, ptyCols, rows, viewportCols, conn.sessionInit, appDefaults);
        else if constexpr (std::is_same_v<T, SshDesc>)
            return std::make_unique<transport::SshTransport>(
                target, desc, ptyCols, rows, viewportCols, conn.sessionInit, appDefaults);
        else if constexpr (std::is_same_v<T, SerialDesc>)
            return std::make_unique<transport::SerialTransport>(
                target, desc, conn.sessionInit, appDefaults);
        else
            return std::make_unique<transport::LoopbackTransport>(target);
    }, conn.transport);
}

Session::Session(const Connection& conn,
                 int scrollbackLines,
                 unsigned short cols,
                 unsigned short rows,
                 std::function<void(transport::DisconnectReason)> onDisconnect,
                 std::function<void(const transport::TransportError&)> onError,
                 AppSessionDefaults appDefaults,
                 unsigned short ptyLineWidth,
                 bool wrapMode,
                 std::function<void(bool)> onAltScreenChanged,
                 std::function<void(bool)> onX11FwdChanged,
                 std::function<std::vector<std::string>(
                     const transport::KbdIntChallenge&)> onKbdIntChallenge,
                 std::function<void()> onBell,
                 std::function<void(bool)> onCursorVisibilityChanged)
    : transport_(MakeTransport(*this, conn, wrapMode ? cols : ptyLineWidth, rows, cols, appDefaults)),
      main_doc_(std::make_unique<MainScreenDocument>(scrollbackLines)),
      alt_doc_(std::make_unique<AltScreenDocument>(rows, cols)),
      active_doc_(main_doc_.get()),
      parser_(*active_doc_, *this),
      docLayout_(std::make_unique<DocLayout>(*main_doc_, cols, rows)),
      onDisconnect_(std::move(onDisconnect)),
      onError_(std::move(onError)),
      onAltScreenChanged_(std::move(onAltScreenChanged)),
      onX11FwdChanged_(std::move(onX11FwdChanged)),
      onKbdIntChallenge_(std::move(onKbdIntChallenge)),
      onBell_(std::move(onBell)),
      onCursorVisibilityChanged_(std::move(onCursorVisibilityChanged)),
      lastCols_(cols),
      lastRows_(rows),
      ptyLineWidth_(ptyLineWidth)
{
    docLayout_->SetWrapMode(wrapMode);
    main_doc_->SetPtyCols(wrapMode ? cols : ptyLineWidth);
    transport_->Start();

    // Submit any profile-configured port forwards after the transport is running.
    if (transport_->SupportsPortForwarding()) {
        if (const auto* ssh = std::get_if<SshDesc>(&conn.transport)) {
            for (auto pf : ssh->portForwards) {
                pf.id = nextPfwId_++;
                portForwardDescs_.push_back(pf);
                transport_->AddPortForward(pf);
            }
        }
    }
}

Session::~Session()
{
    transport_->Stop();
}

void Session::Stop()
{
    transport_->Stop();
}

void Session::ReplaceConnection(const Connection& conn,
                                const AppSessionDefaults& appDefaults)
{
    std::lock_guard<std::mutex> lk(docMutex_);
    // Old transport is already stopped — its worker thread joined before
    // OnDisconnect() propagated to us. Reset it before creating the new one.
    transport_.reset();
    main_doc_->AdvanceCanvas();   // fresh canvas — prior output stays in scrollback
    const bool wrapMode = docLayout_->GetWrapMode();
    transport_ = MakeTransport(*this, conn,
                               wrapMode ? lastCols_ : ptyLineWidth_,
                               lastRows_, lastCols_, appDefaults);
    status_.store(SessionStatus::Reconnecting, std::memory_order_release);
    transport_->Start();

    // Re-submit all configured port forwards to the new transport.
    // IDs are preserved so UIManager's panel can reconcile status updates.
    // Clear the stale status snapshot so callers see an empty list until the
    // transport confirms each forward's state on the new connection.
    if (transport_->SupportsPortForwarding() && !portForwardDescs_.empty()) {
        {
            std::lock_guard<std::mutex> lk(pfwStatusMtx_);
            portForwardStatus_.clear();
        }
        for (const auto& pf : portForwardDescs_)
            transport_->AddPortForward(pf);
    }
}

void Session::ForceAltScreen(bool on)
{
    // UI-thread entry point: exclude the read thread (mid-parse it may be
    // using active_doc_ / the parser doc target this call swaps).
    std::lock_guard<std::mutex> lk(docMutex_);
    if (on == altScreenActive_) return;
    if (on) OnEnterAltScreen();
    else    OnExitAltScreen();
}

void Session::OnInput(const input::KeyEvent& event)
{
    auto bytes = encoder_.Encode(event);
    if (!bytes.empty()) {
        // Any keypress re-enables auto-scroll so the viewport follows output.
        docLayout_->ScrollToEnd();
        transport_->Write(bytes);
    }
}

void Session::Paste(const std::string& utf8)
{
    if (utf8.empty()) return;
    docLayout_->ScrollToEnd();
    if (bracketedPaste_) {
        transport_->Write("\x1b[200~" + utf8 + "\x1b[201~");
    } else {
        transport_->Write(utf8);
    }
}

void Session::OnSetBracketedPaste(bool enabled)
{
    bracketedPaste_ = enabled;
}

void Session::OnData(const std::string& data)
{
    if (status_.load(std::memory_order_acquire) == SessionStatus::Reconnecting)
        status_.store(SessionStatus::Connected, std::memory_order_release);
    std::string response;
    {
        // One lock per chunk (not per byte): excludes UI-thread document
        // mutators for the duration of the parse so the document has one
        // mutator at a time.
        std::lock_guard<std::mutex> lk(docMutex_);
        parser_.Process(data);
        response.swap(pendingResponse_);
    }
    // Flush parse-generated replies (DSR) outside the lock: a synchronous
    // loopback echo re-enters OnData, which must take docMutex_ afresh.
    if (!response.empty())
        transport_->Write(response);
}

void Session::OnError(const transport::TransportError& error)
{
    if (onError_)
        onError_(error);
}

void Session::OnDisconnect(transport::DisconnectReason reason)
{
    status_.store(SessionStatus::Disconnected, std::memory_order_release);
    if (onDisconnect_)
        onDisconnect_(reason);
}

void Session::OnX11StateChanged(bool active)
{
    if (onX11FwdChanged_)
        onX11FwdChanged_(active);
}

std::vector<std::string> Session::OnKbdIntChallenge(
    const transport::KbdIntChallenge& challenge)
{
    if (onKbdIntChallenge_)
        return onKbdIntChallenge_(challenge);
    return {};
}

void Session::RequestX11Forwarding()
{
    transport_->RequestX11Forwarding();
}

void Session::OnPortForwardStatusChanged(std::vector<transport::PortForwardStatus> status)
{
    {
        std::lock_guard<std::mutex> lk(pfwStatusMtx_);
        portForwardStatus_ = status;
    }
    if (onPortForwardChanged_) onPortForwardChanged_(std::move(status));
}

bool Session::SupportsPortForwarding() const
{
    return transport_->SupportsPortForwarding();
}

void Session::SetPortForwardChangedCallback(
    std::function<void(std::vector<transport::PortForwardStatus>)> cb)
{
    onPortForwardChanged_ = std::move(cb);
}

std::vector<transport::PortForwardStatus> Session::GetPortForwardStatus() const
{
    std::lock_guard<std::mutex> lk(pfwStatusMtx_);
    return portForwardStatus_;
}

transport::PortForwardId Session::AddPortForward(transport::PortForwardDesc desc)
{
    desc.id = nextPfwId_++;
    portForwardDescs_.push_back(desc);
    transport_->AddPortForward(desc);
    return desc.id;
}

void Session::RemovePortForward(transport::PortForwardId id)
{
    portForwardDescs_.erase(
        std::remove_if(portForwardDescs_.begin(), portForwardDescs_.end(),
                       [id](const transport::PortForwardDesc& d) { return d.id == id; }),
        portForwardDescs_.end());
    transport_->RemovePortForward(id);
}

ScrollbackSnapshot Session::CaptureScrollback() const
{
    auto snap = main_doc_->CaptureLines();
    const auto now   = std::chrono::system_clock::now();
    const auto ttime = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&ttime), "%Y-%m-%d %H:%M:%S");
    snap.savedAt = ss.str();
    return snap;
}

void Session::LoadScrollback(const ScrollbackSnapshot& snap)
{
    std::lock_guard<std::mutex> lk(docMutex_);
    main_doc_->LoadScrollback(snap);
}

void Session::ResetTerminal(bool clearScrollback)
{
    // UI-thread entry point: the read thread must not be mid-parse while we
    // wipe content, reset the parser FSM, and restore main-screen mode.
    std::lock_guard<std::mutex> lk(docMutex_);
    if (altScreenActive_) OnExitAltScreen();
    bracketedPaste_ = false;
    if (clearScrollback) {
        main_doc_->FullReset(true);   // wipe all content; fires CanvasReset(0)
        if (onClearScrollback_) onClearScrollback_();
    } else {
        main_doc_->SoftReset();       // reset modes only; canvas and content stay
    }
    alt_doc_->FullReset(false);
    parser_.Reset();
    transport_->SendResetSequence();
    if (!cursorVisible_) {
        cursorVisible_ = true;
        if (onCursorVisibilityChanged_) onCursorVisibilityChanged_(true);
    }
}

void Session::OnSetApplicationCursorKeys(bool enabled)
{
    encoder_.SetApplicationCursorKeys(enabled);
}

void Session::OnDeviceStatusReport(int param)
{
    if (param != 6 || !transport_) return;
    const CursorPos pos    = active_doc_->GetCursor();
    const size_t    vStart = altScreenActive_ ? 0 : main_doc_->GetScrollbackOrigin();
    const int row = static_cast<int>(pos.line - vStart) + 1;
    const int col = static_cast<int>(pos.col)           + 1;
    // Buffered, not written: OnData flushes after releasing docMutex_.
    pendingResponse_ += "\x1b[" + std::to_string(row) + ";"
                      + std::to_string(col) + "R";
}

void Session::OnBell()
{
    if (onBell_) onBell_();
}

void Session::OnSetCursorVisibility(bool visible)
{
    if (cursorVisible_ == visible) return;
    cursorVisible_ = visible;
    if (onCursorVisibilityChanged_) onCursorVisibilityChanged_(visible);
}

void Session::OnResetTerminal()
{
    // ESC c — RIS (Reset to Initial State), sent by the remote application.
    // Common sources: tmux/vim on exit, shells on startup, SSH session teardown.
    //
    // We exit alt-screen first (which also resizes the PTY back to ptyLineWidth_
    // in non-wrap mode) so the canvas is always main_doc_ when the reset fires.
    //
    // We then advance the main canvas (FullReset(false)) so prior output stays
    // in scrollback, and resize the transport so the remote's PTY dimensions
    // are re-synchronised after any geometry drift.
    //
    // NOTE: the unconditional resize below may generate a spurious SIGWINCH on
    // the remote side if the dimensions haven't actually changed.  This is
    // intentional: it re-anchors the remote PTY after a full reset.  If this
    // causes visible artifacts (e.g. a TUI immediately re-wrapping), consider
    // gating on lastCols_/lastRows_ having changed.
    if (altScreenActive_) OnExitAltScreen();

    encoder_.SetApplicationCursorKeys(false);
    bracketedPaste_ = false;
    main_doc_->FullReset(false);
    alt_doc_->FullReset(false);

    const int ptyCols = docLayout_->GetWrapMode() ? lastCols_ : ptyLineWidth_;
    transport_->Resize(static_cast<unsigned short>(ptyCols), lastRows_);

    if (!cursorVisible_) {
        cursorVisible_ = true;
        if (onCursorVisibilityChanged_) onCursorVisibilityChanged_(true);
    }

    // parser_.Reset() is called by HandleEscape immediately after this returns
}

void Session::OnEnterAltScreen()
{
    alt_doc_->Resize(lastRows_, lastCols_);
    for (auto* l : externalListeners_)
        main_doc_->RemoveListener(l);
    active_doc_ = alt_doc_.get();
    parser_.SetDocTarget(active_doc_);
    docLayout_->SetDocument(*alt_doc_);
    for (auto* l : externalListeners_)
        alt_doc_->AddListener(l);
    if (!docLayout_->GetWrapMode())
        transport_->Resize(lastCols_, lastRows_);
    altScreenActive_ = true;
    if (onAltScreenChanged_) onAltScreenChanged_(true);
}

void Session::OnExitAltScreen()
{
    for (auto* l : externalListeners_)
        alt_doc_->RemoveListener(l);
    active_doc_ = main_doc_.get();
    parser_.SetDocTarget(active_doc_);
    docLayout_->SetDocument(*main_doc_);
    for (auto* l : externalListeners_)
        main_doc_->AddListener(l);
    if (!docLayout_->GetWrapMode())
        transport_->Resize(ptyLineWidth_, lastRows_);
    altScreenActive_ = false;
    if (onAltScreenChanged_) onAltScreenChanged_(false);
}

void Session::AddDocumentListener(IDocumentListener* listener)
{
    std::lock_guard<std::mutex> lk(docMutex_);
    active_doc_->AddListener(listener);
    externalListeners_.push_back(listener);
}

void Session::RemoveDocumentListener(IDocumentListener* listener)
{
    std::lock_guard<std::mutex> lk(docMutex_);
    active_doc_->RemoveListener(listener);
    externalListeners_.erase(
        std::remove(externalListeners_.begin(), externalListeners_.end(), listener),
        externalListeners_.end());
}

DocLayout& Session::GetDocLayout()
{
    return *docLayout_;
}

void Session::SetTopRow(int row)
{
    docLayout_->SetTopRow(row);
}

void Session::SetViewportSize(unsigned short cols, unsigned short rows)
{
    // UI-thread entry point: SetPtyCols / alt_doc_->Resize mutate state the
    // parser reads mid-Process.
    std::lock_guard<std::mutex> lk(docMutex_);
    docLayout_->SetViewportSize(static_cast<int>(cols), static_cast<int>(rows));
    if (cols == lastCols_ && rows == lastRows_) return;
    if (cols != lastCols_)
        transport_->OnViewportColsChanged(cols);
    lastCols_ = cols;
    lastRows_ = rows;
    if (altScreenActive_) {
        alt_doc_->Resize(rows, cols);
        transport_->Resize(cols, rows);
    } else {
        const bool wrapMode = docLayout_->GetWrapMode();
        const int  ptyCols  = wrapMode ? cols : ptyLineWidth_;
        transport_->Resize(static_cast<unsigned short>(ptyCols), rows);
        main_doc_->SetPtyCols(ptyCols);
    }
}

void Session::SetWrapMode(bool wrap)
{
    std::lock_guard<std::mutex> lk(docMutex_);
    docLayout_->SetWrapMode(wrap);
    if (!altScreenActive_) {
        const int ptyCols = wrap ? lastCols_ : ptyLineWidth_;
        transport_->Resize(static_cast<unsigned short>(ptyCols), lastRows_);
        main_doc_->SetPtyCols(ptyCols);
    }
}

void Session::OnCwdChanged(const std::string& path)
{
    std::lock_guard<std::mutex> lk(cwdMutex_);
    lastCwd_ = path;
}

std::string Session::GetCurrentWorkingDir() const
{
    {
        std::lock_guard<std::mutex> lk(cwdMutex_);
        if (!lastCwd_.empty())
            return lastCwd_;
    }
    return transport_->GetCurrentWorkingDir();
}

bool Session::SupportsFileTransfer() const
{
    return transport_->SupportsFileTransfer();
}

bool Session::SupportsX11Forwarding() const
{
    return transport_->SupportsX11Forwarding();
}


std::string Session::GetTransportRemoteDescription() const
{
    return transport_->GetRemoteDescription();
}

void Session::SendFile(const std::string& localPath,
                       const std::string& remoteDir,
                       std::function<void(bool, std::string)> onDone)
{
    transport_->SendFile(localPath, remoteDir, std::move(onDone));
}

void Session::ReceiveFile(const std::string& remotePath,
                          const std::string& localDir,
                          std::function<void(bool, std::string)> onDone)
{
    transport_->ReceiveFile(remotePath, localDir, std::move(onDone));
}

void Session::ListRemoteDirectory(
    const std::string& remotePath,
    std::function<void(std::vector<transport::RemoteDirEntry>, std::string)> onDone)
{
    transport_->ListRemoteDirectory(remotePath, std::move(onDone));
}

void Session::SftpDownloadFile(const std::string& remotePath,
                               const std::string& localPath,
                               std::function<void(bool, std::string)> onDone)
{
    transport_->SftpDownloadFile(remotePath, localPath, std::move(onDone));
}

void Session::SftpUploadFile(const std::string& localPath,
                             const std::string& remotePath,
                             std::function<void(bool, std::string)> onDone)
{
    transport_->SftpUploadFile(localPath, remotePath, std::move(onDone));
}

} // namespace term::session
