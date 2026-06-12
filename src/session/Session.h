#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "input/InputRouter.h"
#include "transport/AppSessionDefaults.h"
#include "session/Connection.h"
#include "session/InputEncoder.h"
#include "session/SessionStatus.h"
#include "transport/DisconnectReason.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "transport/TransportError.h"
#include "parser/Parser.h"
#include "parser/IScreenTarget.h"
#include "document/Document.h"
#include "layout/DocLayout.h"

namespace term::session {

class Session : public input::InputTarget,
                public parser::IScreenTarget,
                public transport::ITransportTarget {
public:
    Session(const Connection& conn,
            int scrollbackLines,
            unsigned short cols,
            unsigned short rows,
            std::function<void(transport::DisconnectReason)> onDisconnect,
            std::function<void(const transport::TransportError&)> onError,
            transport::AppSessionDefaults appDefaults = {},
            unsigned short ptyLineWidth = 1024,
            bool wrapMode = false,
            std::function<void(bool)> onAltScreenChanged = {},
            std::function<void(bool)> onX11FwdChanged = {},
            std::function<std::vector<std::string>(
                const transport::KbdIntChallenge&)> onKbdIntChallenge = {},
            std::function<void()> onBell = {},
            std::function<void(bool)> onCursorVisibilityChanged = {});
    ~Session();

    // Stops the transport I/O thread synchronously. Safe to call multiple
    // times. SessionManager calls this before erasing the SessionRecord so
    // that no DocumentObserver callbacks are in-flight during destruction.
    void Stop();

    // Replaces the dead transport with a fresh one built from conn and starts it.
    // The old transport must already be stopped (i.e. OnDisconnect() was received).
    // Document, DocLayout, and all UI state are preserved.
    void ReplaceConnection(const Connection& conn,
                           const transport::AppSessionDefaults& appDefaults);

    SessionStatus GetStatus() const { return status_.load(std::memory_order_acquire); }

    // input::InputTarget
    void OnInput(const input::KeyEvent& event) override;
    void Paste(const std::string& utf8) override;

    // Resets all terminal state (modes, attributes, parser FSM).
    // If clearScrollback=true, also wipes all content from main_doc_.
    // Sends \021 (XON) + \033c to the transport to unblock flow control
    // and notify the running process.
    void ResetTerminal(bool clearScrollback);

    // parser::IScreenTarget
    void OnEnterAltScreen()                          override;
    void OnExitAltScreen()                           override;
    void OnResetTerminal()                           override;
    void OnSetApplicationCursorKeys(bool enabled)    override;
    void OnSetBracketedPaste(bool enabled)           override;
    void OnDeviceStatusReport(int param)             override;
    void OnBell()                                    override;
    void OnSetCursorVisibility(bool visible)         override;
    void OnCwdChanged(const std::string& path)       override;

    // transport::ITransportTarget
    void OnData(const std::string& data) override;
    void OnError(const transport::TransportError& error) override;
    void OnDisconnect(transport::DisconnectReason reason) override;
    void OnX11StateChanged(bool active) override;
    void OnPortForwardStatusChanged(std::vector<transport::PortForwardStatus> status) override;
    std::vector<std::string> OnKbdIntChallenge(
        const transport::KbdIntChallenge& challenge) override;

    // Allows SessionManager to attach a DocumentObserver to the active document.
    void AddDocumentListener(IDocumentListener* listener);
    void RemoveDocumentListener(IDocumentListener* listener);

    // Returned by value — Document::GetTitle copies under its title mutex.
    std::string GetTitle() const { return main_doc_->GetTitle(); }
    DocLayout& GetDocLayout();

    // Layout forwarding — UIManager routes viewport events through here.
    // Both flags are atomics: written on the transport read thread (parser
    // callbacks), read lock-free from the UI thread (menu guards, Paste).
    bool IsAltScreenActive()    const { return altScreenActive_; }
    bool IsBracketedPasteActive() const { return bracketedPaste_; }
    void ForceAltScreen(bool on);

    // Enqueues an X11 forwarding request on the underlying SSH transport.
    // No-op for non-SSH sessions.
    void RequestX11Forwarding();

    // Port forwarding — SSH only; no-op for PTY/Serial.
    bool SupportsPortForwarding() const;
    transport::PortForwardId AddPortForward(transport::PortForwardDesc desc);  // assigns next id, calls transport; returns id
    void RemovePortForward(transport::PortForwardId id);
    const std::vector<transport::PortForwardDesc>& GetPortForwardDescs() const { return portForwardDescs_; }
    // Returns a by-value snapshot (mutex-protected; safe to call from the UI thread
    // while the transport worker may concurrently update the status).
    std::vector<transport::PortForwardStatus> GetPortForwardStatus() const;

    // Registered by UIManager once via TakeSession.  Fired on the transport worker
    // thread; implementations must marshal to the UI thread before touching wx.
    void SetPortForwardChangedCallback(
        std::function<void(std::vector<transport::PortForwardStatus>)> cb);

    void SetTopRow(int row);
    void SetViewportSize(unsigned short cols, unsigned short rows);
    void SetWrapMode(bool wrap);

    // Returns a reference to the main-screen document (scrollback history).
    // Used by ScrollbackWriter to read lines on InsertLine notifications.
    const Document& GetMainDoc() const { return *main_doc_; }

    // Captures a thread-safe snapshot of the current main-screen scrollback.
    // savedAt is set to the current wall-clock time; caller may persist it.
    ScrollbackSnapshot CaptureScrollback() const;

    // Injects a previously captured snapshot into the main document, prepending
    // the restored lines and a separator before the current canvas.
    void LoadScrollback(const ScrollbackSnapshot& snap);

    // Wired by SessionManager to ScrollbackWriter::Truncate().
    // Called inside ResetTerminal(clearScrollback=true).
    std::function<void()> onClearScrollback_;

    std::string GetCurrentWorkingDir() const;
    bool        SupportsFileTransfer()   const;
    bool        SupportsX11Forwarding() const;
    std::string GetTransportRemoteDescription() const;
    void        SendFile(const std::string& localPath,
                        const std::string& remoteDir,
                        std::function<void(bool, std::string)> onDone);
    void        ReceiveFile(const std::string& remotePath,
                            const std::string& localDir,
                            std::function<void(bool, std::string)> onDone);
    void        ListRemoteDirectory(
                    const std::string& remotePath,
                    std::function<void(std::vector<transport::RemoteDirEntry>,
                                       std::string)> onDone);
    void        SftpDownloadFile(const std::string& remotePath,
                                 const std::string& localPath,
                                 std::function<void(bool, std::string)> onDone);
    void        SftpUploadFile(const std::string& localPath,
                               const std::string& remotePath,
                               std::function<void(bool, std::string)> onDone);

private:
    static std::unique_ptr<transport::Transport> MakeTransport(
        transport::ITransportTarget& target,
        const Connection& conn,
        unsigned short ptyCols,
        unsigned short rows,
        unsigned short viewportCols,
        const transport::AppSessionDefaults& appDefaults);

private:
    std::unique_ptr<transport::Transport> transport_;
    InputEncoder                          encoder_;

    std::unique_ptr<MainScreenDocument> main_doc_;
    std::unique_ptr<Document>           alt_doc_;
    Document*                 active_doc_;

    parser::Parser                        parser_;

    std::unique_ptr<DocLayout> docLayout_;

    std::function<void(transport::DisconnectReason)>         onDisconnect_;
    std::function<void(const transport::TransportError&)>    onError_;
    std::function<void(bool)>                                onAltScreenChanged_;
    std::function<void(bool)>                                onX11FwdChanged_;
    std::function<std::vector<std::string>(
        const transport::KbdIntChallenge&)>                  onKbdIntChallenge_;
    std::function<void()>                                    onBell_;
    std::function<void(bool)>                                onCursorVisibilityChanged_;

    // Serialises structural document/parser mutations between the transport
    // read thread (OnData → parser_.Process) and the UI-thread entry points
    // that mutate the document model directly (ResetTerminal, ForceAltScreen,
    // LoadScrollback, SetViewportSize, SetWrapMode, listener registration,
    // ReplaceConnection). Restores the single-mutator invariant the Document
    // layer's unlocked writer-side reads rely on. The paint path deliberately
    // does NOT take this — UI reads go through Document::linesMutex_, so
    // rendering never blocks behind a parse chunk.
    mutable std::mutex docMutex_;

    // Guards lastCwd_ alone. OnCwdChanged also fires from the transport worker
    // outside the OnData path (periodic /proc capture), so it cannot share
    // docMutex_ without deadlocking the OSC 7 path (OnData already holds it).
    mutable std::mutex cwdMutex_;

    // Response bytes generated during a parse (DSR replies). Accumulated under
    // docMutex_ and written to the transport only after the lock is released —
    // LoopbackTransport echoes Write() synchronously on the calling thread, so
    // writing mid-parse would re-enter OnData and self-deadlock on docMutex_.
    std::string pendingResponse_;

    std::string        lastCwd_;
    unsigned short     lastCols_{0};
    unsigned short     lastRows_{0};
    unsigned short     ptyLineWidth_{1024};
    // Written under docMutex_ (parser callbacks or UI mutators); atomic so the
    // UI thread can read them lock-free at any time.
    std::atomic<bool>  altScreenActive_{false};
    std::atomic<bool>  bracketedPaste_{false};
    std::atomic<bool>  cursorVisible_{true};
    std::atomic<SessionStatus> status_{SessionStatus::Connected};
    // Guarded by docMutex_: registered from the UI thread, iterated on the
    // transport read thread during alt-screen switches.
    std::vector<IDocumentListener*> externalListeners_;

    // Port forward state.
    // portForwardDescs_ is UI-thread-only (written only from AddPortForward /
    // RemovePortForward which are called on the UI thread).
    // portForwardStatus_ is written on the transport worker thread and read on the
    // UI thread; all accesses must hold pfwStatusMtx_.
    std::vector<transport::PortForwardDesc>   portForwardDescs_;
    mutable std::mutex                        pfwStatusMtx_;
    std::vector<transport::PortForwardStatus> portForwardStatus_;
    transport::PortForwardId                  nextPfwId_{1};
    std::function<void(std::vector<transport::PortForwardStatus>)> onPortForwardChanged_;
};

} // namespace term::session
