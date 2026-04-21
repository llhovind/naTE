#pragma once

#include <memory>
#include <functional>

#include "input/InputRouter.h"
#include "session/InputEncoder.h"
#include "transport/Transport.hpp"
#include "parser/Parser.h"
#include "parser/IParserTarget.h"
#include "document/Document.h"
#include "ui/Layout.h"

namespace term::session {

class Session : public input::InputTarget, public parser::IParserTarget {
public:
    using RefreshCallback = std::function<void()>;

    explicit Session(std::unique_ptr<transport::Transport> transport);

    // input::InputTarget
    void OnInput(const input::KeyEvent& event) override;

    // parser::IParserTarget
    void OnAppendChar(char32_t ch) override;
    void OnNewLine() override;
    void OnSetStyle(const Style& style) override;

    void SetRefreshCallback(RefreshCallback cb);

    Layout& GetLayout();

private:
    void OnTransportData(const std::string& data);

private:
    std::unique_ptr<transport::Transport> transport_;
    InputEncoder                          encoder_;
    parser::Parser                        parser_;

    std::unique_ptr<Document> main_doc_;
    std::unique_ptr<Document> alt_doc_;
    Document*                 active_doc_;

    std::unique_ptr<Layout>   layout_;

    RefreshCallback           refresh_callback_;
};

} // namespace term::session
