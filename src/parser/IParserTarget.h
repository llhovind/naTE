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
    virtual void OnCarriageReturn() = 0;
    virtual void OnSetStyle(const Style& style) = 0;
    virtual void OnSetTitle(const std::string& /*title*/) {}

    virtual void OnCursorUp(int /*count*/)              {}
    virtual void OnCursorDown(int /*count*/)            {}
    virtual void OnCursorRight(int /*count*/)           {}
    virtual void OnCursorLeft(int /*count*/)            {}
    virtual void OnEraseInLine(int /*mode*/)            {}

    // Cursor positioning
    virtual void OnCursorPosition(int /*row*/, int /*col*/) {}  // 1-indexed
    virtual void OnCursorToLineStart()                  {}
    virtual void OnCursorEnd()                          {}

    // Screen / line editing
    virtual void OnEraseInDisplay(int /*mode*/)         {}
    virtual void OnDeleteChar(int /*count*/)            {}

    // Cursor visibility and alternate screen
    virtual void OnSetCursorVisibility(bool /*visible*/) {}
    virtual void OnEnterAltScreen()                     {}
    virtual void OnExitAltScreen()                      {}

    // Function keys and scroll (no-op at target level by default)
    virtual void OnFunctionKey(int /*n*/)               {}
    virtual void OnScrollUp(int /*count*/)              {}
    virtual void OnScrollDown(int /*count*/)            {}
};

} // namespace term::parser
