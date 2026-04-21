#pragma once

#include "document/Document.h"

namespace term::parser {

class IParserTarget {
public:
    virtual ~IParserTarget() = default;

    virtual void OnAppendInsertChar(char32_t ch) = 0;
    virtual void OnNewLine() = 0;
    virtual void OnSetStyle(const Style& style) = 0;
};

} // namespace term::parser
