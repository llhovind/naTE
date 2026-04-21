#pragma once

#include "parser/IParserTarget.h"
#include <string>

namespace term::parser {

class Parser {
public:
    explicit Parser(IParserTarget& target);

    void Process(const std::string& data);

private:
    enum class State { Normal, Escape, Csi };

    void HandleNormal(unsigned char byte);
    void HandleEscape(unsigned char byte);
    void HandleCsi(unsigned char byte);

    void DispatchSgr();

    IParserTarget& target_;
    State          state_ = State::Normal;
    std::string    params_;  // CSI parameter accumulator

    // UTF-8 multi-byte decode state
    char32_t utf8_codepoint_ = 0;
    int      utf8_remaining_ = 0;
};

} // namespace term::parser
