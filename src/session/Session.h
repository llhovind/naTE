#pragma once

#include <functional>
#include <memory>

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
            std::function<void()> onDisconnect);
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
    void OnSetStyle(const Style& style) override;
    void OnSetTitle(const std::string& title) override;

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
};

} // namespace term::session
