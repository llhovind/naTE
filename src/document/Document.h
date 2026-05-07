#pragma once

#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include "document/IDocumentListener.h"

struct Style {
    int fg = -1;  // -1 = use terminal default
    int bg = -1;  // -1 = use terminal default
    bool bold = false;

    bool operator==(const Style& other) const {
        return fg == other.fg && bg == other.bg && bold == other.bold;
    }

    bool operator!=(const Style& other) const {
        return !(*this == other);
    }
};

struct StyleRun {
    size_t start;
    size_t length;
    Style style;
};

struct CursorPos {
    size_t line{0};
    size_t col{0};
};

struct DocLine {
    std::u32string text;
    std::vector<StyleRun> styles;
    Style currentStyle;

    void WriteAt(size_t col, char32_t ch);
    void InsertAt(size_t col, char32_t ch);
    void DeletePreviousChar(size_t cursorCol);
    void DeleteAt(size_t col, size_t count);
    void Clear();
};

// Interface through which the Parser drives document mutations directly,
// eliminating the Session pass-through layer.
class IDocumentTarget {
public:
    virtual ~IDocumentTarget() = default;

    virtual void AppendInsertChar(char32_t ch) = 0;
    virtual void Backspace() = 0;
    virtual void NewLine() = 0;
    virtual void CarriageReturn() = 0;
    virtual void SetCurrentStyle(const Style& style) = 0;
    virtual void SetTitle(const std::string& /*title*/) {}

    virtual void MoveCursorLeft(int n) = 0;
    virtual void MoveCursorRight(int n) = 0;
    virtual void MoveCursorUp(int n) = 0;
    virtual void MoveCursorDown(int n) = 0;
    virtual void MoveCursorToLineStart() = 0;
    virtual void MoveCursorToLineEnd() = 0;
    virtual void MoveCursorToPosition(int row, int col) = 0;
    virtual void MoveCursorToColumn(int /*col*/) {}
    virtual void MoveCursorToRow(int /*row*/) {}
    virtual void SaveCursor() {}
    virtual void RestoreCursor() {}

    virtual void EraseInLine(int mode) = 0;
    virtual void EraseInDisplay(int mode) = 0;
    virtual void EraseChar(int /*count*/) {}
    virtual void DeleteChar(int count) = 0;
    virtual void InsertChar(int count) = 0;
    virtual void SetInsertMode(bool /*on*/) {}

    virtual void ReverseIndex() {}
    virtual void SetScrollRegion(int /*top*/, int /*bot*/) {}
    virtual void InsertLines(int /*count*/) {}
    virtual void DeleteLines(int /*count*/) {}
};

class Document : public IDocumentTarget {
public:
    virtual ~Document() = default;

    virtual void AppendInsertChar(char32_t ch) = 0;
    virtual void Backspace() = 0;
    virtual void NewLine() = 0;
    virtual void CarriageReturn() = 0;
    virtual void SetCurrentStyle(const Style& style) = 0;
    virtual const std::deque<DocLine>& GetLines() const = 0;

    virtual void MoveCursorLeft(int n)  = 0;
    virtual void MoveCursorRight(int n) = 0;
    virtual void MoveCursorUp(int n)    = 0;
    virtual void MoveCursorDown(int n)  = 0;
    virtual void EraseInLine(int mode)  = 0;

    virtual void MoveCursorToLineStart()               = 0;
    virtual void MoveCursorToLineEnd()                 = 0;
    virtual void MoveCursorToPosition(int row, int col) = 0;  // 1-indexed
    virtual void DeleteChar(int count)                 = 0;
    virtual void EraseInDisplay(int mode)              = 0;

    virtual void Resize(int rows, int cols) {}           // no-op default; AltScreenDocument overrides
    virtual void ReverseIndex() {}                       // ESC M — scroll region content down
    virtual void SetScrollRegion(int top, int bot) {}    // CSI r — set top/bottom margins (1-indexed)
    virtual void InsertLines(int count) {}               // CSI Ps L — insert blank lines at cursor
    virtual void DeleteLines(int count) {}               // CSI Ps M — delete lines at cursor
    virtual void MoveCursorToColumn(int col) {}          // CSI G — cursor column absolute (1-indexed)
    virtual void MoveCursorToRow(int row) {}             // CSI d — cursor row absolute (1-indexed)
    // PTY-configured column width (what readline was told). Used by
    // MainScreenDocument to map readline's virtual (row,col) to a flat
    // document column. Defaults are safe; Session keeps this in sync.
    virtual void SetPtyCols(int cols) {}
    virtual void SaveCursor() {}                         // ESC 7 / CSI s
    virtual void RestoreCursor() {}                      // ESC 8 / CSI u
    virtual void EraseChar(int count) {}                 // CSI X — erase chars at cursor, no cursor movement
    virtual void InsertChar(int count) = 0;              // CSI @ — insert blank chars, shift right
    virtual void SetInsertMode(bool on) { insertMode_ = on; }

    CursorPos GetCursor() const { return cursor_; }

    const std::string& GetTitle() const { return title_; }
    void SetTitle(const std::string& title);

    void AddListener(IDocumentListener* listener);
    void RemoveListener(IDocumentListener* listener);

protected:
    void NotifyListeners(DocChangeType type, size_t lineIndex);

    CursorPos  cursor_{};
    std::string title_;
    bool insertMode_ = false;

private:
    std::vector<IDocumentListener*> listeners_;
};

class MainScreenDocument : public Document {
public:
    explicit MainScreenDocument(int maxLines = 100'000);

    void AppendInsertChar(char32_t ch) override;
    void Backspace() override;
    void NewLine() override;
    void CarriageReturn() override;
    void SetCurrentStyle(const Style& style) override;
    const std::deque<DocLine>& GetLines() const override { return lines_; }

    void MoveCursorLeft(int n)  override;
    void MoveCursorRight(int n) override;
    void MoveCursorUp(int n)    override;
    void MoveCursorDown(int n)  override;
    void EraseInLine(int mode)  override;

    void MoveCursorToLineStart()               override;
    void MoveCursorToLineEnd()                 override;
    void MoveCursorToPosition(int row, int col) override;
    void MoveCursorToColumn(int col)           override;
    void MoveCursorToRow(int row)              override;
    void EraseChar(int count)                  override;
    void DeleteChar(int count)                 override;
    void EraseInDisplay(int mode)              override;
    void InsertChar(int count)                 override;
    void InsertLines(int count)                override;
    void DeleteLines(int count)                override;
    void SetPtyCols(int cols)                  override;

private:
    std::deque<DocLine> lines_;
    int maxLines_;
    int cols_ = 2048;  // PTY-configured column count; drives virtual-row cursor translation
};

class AltScreenDocument : public Document {
public:
    AltScreenDocument(int rows, int cols);
    void Resize(int rows, int cols)          override;
    void ReverseIndex()                      override;
    void SetScrollRegion(int top, int bot)   override;
    void MoveCursorToColumn(int col)         override;
    void MoveCursorToRow(int row)            override;
    void SaveCursor()                        override;
    void RestoreCursor()                     override;
    void EraseChar(int count)               override;

    void AppendInsertChar(char32_t ch) override;
    void Backspace() override;
    void NewLine() override;
    void CarriageReturn() override;
    void SetCurrentStyle(const Style& style) override;
    const std::deque<DocLine>& GetLines() const override { return lines_; }

    void MoveCursorLeft(int n)  override;
    void MoveCursorRight(int n) override;
    void MoveCursorUp(int n)    override;
    void MoveCursorDown(int n)  override;
    void EraseInLine(int mode)  override;

    void MoveCursorToLineStart()                override;
    void MoveCursorToLineEnd()                  override;
    void MoveCursorToPosition(int row, int col)  override;
    void DeleteChar(int count)                  override;
    void EraseInDisplay(int mode)               override;
    void InsertChar(int count)                  override;
    void InsertLines(int count)                 override;
    void DeleteLines(int count)                 override;

private:
    int       rows_;
    int       cols_;
    int       scrollTop_   = 0;    // 0-indexed top margin of scroll region
    int       scrollBot_   = 0;    // 0-indexed bottom margin of scroll region
    CursorPos savedCursor_ = {};   // saved by ESC 7 / CSI s
    std::deque<DocLine> lines_;    // always exactly rows_ entries
};
