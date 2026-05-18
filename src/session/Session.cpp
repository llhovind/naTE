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
                target, desc.shell, ptyCols, rows, viewportCols, conn.sessionInit, appDefaults);
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
                 std::function<void()> onDisconnect,
                 std::function<void(const transport::TransportError&)> onError,
                 AppSessionDefaults appDefaults,
                 unsigned short ptyLineWidth,
                 bool wrapMode)
    : transport_(MakeTransport(*this, conn, wrapMode ? cols : ptyLineWidth, rows, cols, appDefaults)),
      main_doc_(std::make_unique<MainScreenDocument>(scrollbackLines)),
      alt_doc_(std::make_unique<AltScreenDocument>(rows, cols)),
      active_doc_(main_doc_.get()),
      parser_(*active_doc_, *this),
      docLayout_(std::make_unique<DocLayout>(*main_doc_, cols, rows)),
      onDisconnect_(std::move(onDisconnect)),
      onError_(std::move(onError)),
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
    transport_->Write(utf8);
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

void Session::OnDisconnect()
{
    if (onDisconnect_)
        onDisconnect_();
}

void Session::ResetTerminal(bool clearScrollback)
{
    if (altScreenActive_) {
        for (auto* l : externalListeners_) alt_doc_->RemoveListener(l);
        active_doc_ = main_doc_.get();
        parser_.SetDocTarget(active_doc_);
        docLayout_->SetDocument(*main_doc_);
        for (auto* l : externalListeners_) main_doc_->AddListener(l);
        if (!docLayout_->GetWrapMode())
            transport_->Resize(ptyLineWidth_, lastRows_);
        altScreenActive_ = false;
    }
    main_doc_->FullReset(clearScrollback);
    alt_doc_->FullReset(false);
    parser_.Reset();
    transport_->Write("\021\033c");
}

void Session::OnResetTerminal()
{
    if (altScreenActive_) {
        for (auto* l : externalListeners_) alt_doc_->RemoveListener(l);
        active_doc_ = main_doc_.get();
        parser_.SetDocTarget(active_doc_);
        docLayout_->SetDocument(*main_doc_);
        for (auto* l : externalListeners_) main_doc_->AddListener(l);
        altScreenActive_ = false;
    }
    main_doc_->FullReset(false);
    alt_doc_->FullReset(false);
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

bool Session::SupportsFileTransfer() const
{
    return transport_->SupportsFileTransfer();
}

std::string Session::GetTransportRemoteDescription() const
{
    return transport_->GetRemoteDescription();
}

void Session::TransferFile(const std::string& localPath,
                           const std::string& remoteDir,
                           std::function<void(bool, std::string)> onDone)
{
    transport_->TransferFile(localPath, remoteDir, std::move(onDone));
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
