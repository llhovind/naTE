#include "session/Session.h"
#include "transport/PtyTransport.h"
#include "transport/LoopbackTransport.h"
#include "transport/SerialTransport.h"
#include "transport/SshTransport.h"
#include <algorithm>

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
    // Old transport is already stopped — its worker thread joined before
    // OnDisconnect() propagated to us. Reset it before creating the new one.
    transport_.reset();
    main_doc_->AdvanceCanvas();   // fresh canvas — prior output stays in scrollback
    const bool wrapMode = docLayout_->GetWrapMode();
    transport_ = MakeTransport(*this, conn,
                               wrapMode ? lastCols_ : ptyLineWidth_,
                               lastRows_, lastCols_, appDefaults);
    status_ = SessionStatus::Reconnecting;
    transport_->Start();
}

void Session::ForceAltScreen(bool on)
{
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
    parser_.Process(data);
}

void Session::OnError(const transport::TransportError& error)
{
    if (onError_)
        onError_(error);
}

void Session::OnDisconnect(transport::DisconnectReason reason)
{
    status_ = SessionStatus::Disconnected;
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

void Session::ResetTerminal(bool clearScrollback)
{
    if (altScreenActive_) OnExitAltScreen();
    bracketedPaste_ = false;
    if (clearScrollback) {
        main_doc_->FullReset(true);   // wipe all content; fires CanvasReset(0)
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
    transport_->Write("\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "R");
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
    active_doc_->AddListener(listener);
    externalListeners_.push_back(listener);
}

void Session::RemoveDocumentListener(IDocumentListener* listener)
{
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
    docLayout_->SetWrapMode(wrap);
    if (!altScreenActive_) {
        const int ptyCols = wrap ? lastCols_ : ptyLineWidth_;
        transport_->Resize(static_cast<unsigned short>(ptyCols), lastRows_);
        main_doc_->SetPtyCols(ptyCols);
    }
}

std::string Session::GetCurrentWorkingDir() const
{
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

} // namespace term::session
