#pragma once

#include <memory>
#include <functional>

#include "input/InputRouter.h"
#include "session/InputEncoder.h"
#include "transport/Transport.hpp"
#include "parser/Parser.h"
#include "parser/IParserTarget.h"
#include "document/Document.h"
#include "ui/DocLayout.h"

namespace term::session {

class Session : public input::InputTarget, public parser::IParserTarget {
public:
    using RefreshCallback    = std::function<void()>;
    using TitleCallback      = std::function<void(const std::string&)>;
    using DisconnectCallback = std::function<void()>;

    explicit Session(std::unique_ptr<transport::Transport> transport,
                     int scrollbackLines = 100'000);

    // input::InputTarget
    void OnInput(const input::KeyEvent& event) override;

    // parser::IParserTarget
    void OnAppendInsertChar(char32_t ch) override;
    void OnBackspace() override;
    void OnNewLine() override;
    void OnSetStyle(const Style& style) override;
    void OnSetTitle(const std::string& title) override;

    void SetRefreshCallback(RefreshCallback cb);
    void SetTitleCallback(TitleCallback cb);
    void SetDisconnectCallback(DisconnectCallback cb);

    const std::string& GetTitle() const { return title_; }
    DocLayout& GetDocLayout();

private:
    void OnTransportData(const std::string& data);

private:
    std::unique_ptr<transport::Transport> transport_;
    InputEncoder                          encoder_;
    parser::Parser                        parser_;

    std::unique_ptr<Document> main_doc_;
    std::unique_ptr<Document> alt_doc_;
    Document*                 active_doc_;

    std::unique_ptr<DocLayout>   docLayout_;

    RefreshCallback    refresh_callback_;
    TitleCallback      title_callback_;
    DisconnectCallback disconnect_callback_;
    std::string        title_;
};

} // namespace term::session
