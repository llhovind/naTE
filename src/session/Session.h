#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "input/InputRouter.h"
#include "session/Connection.h"
#include "session/InputEncoder.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include "parser/Parser.h"
#include "parser/IParserTarget.h"
#include "document/Document.h"
#include "ui/DocLayout.h"

namespace term::session {

class Session : public input::InputTarget,
                public parser::IParserTarget,
                public transport::ITransportTarget {
public:
    Session(const Connection& conn,
            int scrollbackLines,
            unsigned short cols,
            unsigned short rows,
            std::function<void()> onDisconnect,
            unsigned short ptyLineWidth = 1024,
            bool widePty = false,
            bool wordWrap = false);
    ~Session();

    // Stops the transport I/O thread synchronously. Safe to call multiple
    // times. SessionManager calls this before erasing the SessionRecord so
    // that no DocumentObserver callbacks are in-flight during destruction.
    void Stop();

    // input::InputTarget
    void OnInput(const input::KeyEvent& event) override;

    // parser::IParserTarget
    void OnAppendInsertChar(char32_t ch) override;
    void OnBackspace() override;
    void OnNewLine() override;
    void OnCarriageReturn() override;
    void OnSetStyle(const Style& style) override;
    void OnSetTitle(const std::string& title) override;
    void OnCursorUp(int count)              override;
    void OnCursorDown(int count)            override;
    void OnCursorRight(int count)           override;
    void OnCursorLeft(int count)            override;
    void OnEraseInLine(int mode)            override;
    void OnCursorPosition(int row, int col) override;
    void OnCursorToLineStart()              override;
    void OnCursorEnd()                      override;
    void OnEraseInDisplay(int mode)         override;
    void OnDeleteChar(int count)            override;
    void OnEnterAltScreen()                 override;
    void OnExitAltScreen()                  override;
    void OnReverseIndex()                    override;
    void OnSetScrollRegion(int top, int bot) override;
    void OnCursorColumnAbsolute(int col)     override;
    void OnCursorRowAbsolute(int row)        override;
    void OnSaveCursor()                      override;
    void OnRestoreCursor()                   override;
    void OnEraseChar(int count)              override;

    // transport::ITransportTarget
    void OnData(const std::string& data) override;
    void OnDisconnect() override;

    // Allows SessionManager to attach a DocumentObserver to the active document.
    void AddDocumentListener(IDocumentListener* listener);
    void RemoveDocumentListener(IDocumentListener* listener);

    const std::string& GetTitle() const { return main_doc_->GetTitle(); }
    DocLayout& GetDocLayout();

    // Layout forwarding — UIManager routes viewport events through here.
    void SetTopRow(int row);
    void SetViewportSize(unsigned short cols, unsigned short rows);

private:
    static std::unique_ptr<transport::Transport> MakeTransport(
        transport::ITransportTarget& target,
        const Connection& conn,
        unsigned short cols,
        unsigned short rows);

private:
    std::unique_ptr<transport::Transport> transport_;
    InputEncoder                          encoder_;
    parser::Parser                        parser_;

    std::unique_ptr<Document> main_doc_;
    std::unique_ptr<Document> alt_doc_;
    Document*                 active_doc_;

    std::unique_ptr<DocLayout> docLayout_;

    std::function<void()> onDisconnect_;

    unsigned short     lastCols_{0};
    unsigned short     lastRows_{0};
    unsigned short     ptyLineWidth_{1024};
    const bool         widePty_{false};
    bool               altScreenActive_{false};
    std::vector<IDocumentListener*> externalListeners_;
};

} // namespace term::session
