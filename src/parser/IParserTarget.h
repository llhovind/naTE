#pragma once

#include "document/Document.h"
#include <string>

namespace term::parser {

class IParserTarget {
public:
    virtual ~IParserTarget() = default;

    virtual void OnAppendInsertChar(char32_t ch) = 0;
    virtual void OnBackspace() = 0;
    virtual void OnNewLine() = 0;
    virtual void OnSetStyle(const Style& style) = 0;
    virtual void OnSetTitle(const std::string& /*title*/) {}
};

} // namespace term::parser
