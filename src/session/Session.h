#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/InputRouter.h"
#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "session/InputEncoder.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "transport/TransportError.h"
#include "parser/Parser.h"
#include "parser/IScreenTarget.h"
#include "document/Document.h"
#include "ui/DocLayout.h"

namespace term::session {

class Session : public input::InputTarget,
                public parser::IScreenTarget,
                public transport::ITransportTarget {
public:
    Session(const Connection& conn,
            int scrollbackLines,
            unsigned short cols,
            unsigned short rows,
            std::function<void()> onDisconnect,
            std::function<void(const transport::TransportError&)> onError,
            AppSessionDefaults appDefaults = {},
            unsigned short ptyLineWidth = 1024,
            bool wrapMode = false,
            std::function<void(bool)> onAltScreenChanged = {},
            std::function<void(bool)> onX11FwdChanged = {},
            std::function<std::vector<std::string>(
                const transport::KbdIntChallenge&)> onKbdIntChallenge = {},
            std::function<void()> onBell = {});
    ~Session();

    // Stops the transport I/O thread synchronously. Safe to call multiple
    // times. SessionManager calls this before erasing the SessionRecord so
    // that no DocumentObserver callbacks are in-flight during destruction.
    void Stop();

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
    void OnBell()                                    override;

    // transport::ITransportTarget
    void OnData(const std::string& data) override;
    void OnError(const transport::TransportError& error) override;
    void OnDisconnect() override;
    void OnX11StateChanged(bool active) override;
    std::vector<std::string> OnKbdIntChallenge(
        const transport::KbdIntChallenge& challenge) override;

    // Allows SessionManager to attach a DocumentObserver to the active document.
    void AddDocumentListener(IDocumentListener* listener);
    void RemoveDocumentListener(IDocumentListener* listener);

    const std::string& GetTitle() const { return main_doc_->GetTitle(); }
    DocLayout& GetDocLayout();

    // Layout forwarding — UIManager routes viewport events through here.
    bool IsAltScreenActive()    const { return altScreenActive_; }
    bool IsBracketedPasteActive() const { return bracketedPaste_; }
    void ForceAltScreen(bool on);

    // Enqueues an X11 forwarding request on the underlying SSH transport.
    // No-op for non-SSH sessions.
    void RequestX11Forwarding();

    void SetTopRow(int row);
    void SetViewportSize(unsigned short cols, unsigned short rows);
    void SetWrapMode(bool wrap);

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

private:
    static std::unique_ptr<transport::Transport> MakeTransport(
        transport::ITransportTarget& target,
        const Connection& conn,
        unsigned short ptyCols,
        unsigned short rows,
        unsigned short viewportCols,
        const AppSessionDefaults& appDefaults);

private:
    std::unique_ptr<transport::Transport> transport_;
    InputEncoder                          encoder_;

    std::unique_ptr<Document> main_doc_;
    std::unique_ptr<Document> alt_doc_;
    Document*                 active_doc_;

    parser::Parser                        parser_;

    std::unique_ptr<DocLayout> docLayout_;

    std::function<void()>                                    onDisconnect_;
    std::function<void(const transport::TransportError&)>    onError_;
    std::function<void(bool)>                                onAltScreenChanged_;
    std::function<void(bool)>                                onX11FwdChanged_;
    std::function<std::vector<std::string>(
        const transport::KbdIntChallenge&)>                  onKbdIntChallenge_;
    std::function<void()>                                    onBell_;

    unsigned short     lastCols_{0};
    unsigned short     lastRows_{0};
    unsigned short     ptyLineWidth_{1024};
    bool               altScreenActive_{false};
    bool               bracketedPaste_{false};
    std::vector<IDocumentListener*> externalListeners_;
};

} // namespace term::session
