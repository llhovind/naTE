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

    // VT100 scroll region and reverse index
    virtual void OnReverseIndex()                                {}  // ESC M
    virtual void OnSetScrollRegion(int /*top*/, int /*bot*/)     {}  // CSI top;bot r
    virtual void OnInsertLines(int /*count*/)                    {}  // CSI Ps L
    virtual void OnDeleteLines(int /*count*/)                    {}  // CSI Ps M

    // Cursor positioning (column/row absolute) and save/restore
    virtual void OnCursorColumnAbsolute(int /*col*/)             {}  // CSI G (1-indexed)
    virtual void OnCursorRowAbsolute(int /*row*/)                {}  // CSI d (1-indexed)
    virtual void OnSaveCursor()                                  {}  // ESC 7 or CSI s
    virtual void OnRestoreCursor()                               {}  // ESC 8 or CSI u
    virtual void OnEraseChar(int /*count*/)                      {}  // CSI X
    virtual void OnInsertChar(int /*count*/)                     {}  // CSI @
    virtual void OnSetInsertMode(bool /*on*/)                    {}  // CSI 4 h/l
};

} // namespace term::parser
