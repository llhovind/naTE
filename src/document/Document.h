#pragma once

#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <cstdint>
#include "config/Color.h"
#include "document/IDocumentListener.h"

// Written into the cell immediately right of a wide (2-column) character so
// that the slot is claimed and the renderer can skip it.  U+FFFE is a
// guaranteed non-character that never appears in valid terminal output.
inline constexpr char32_t kWideFiller = U'￾';

// Returns the number of terminal columns occupied by codepoint cp: 2 for
// East-Asian Wide/Fullwidth characters, 1 for everything else printable.
// Does not rely on setlocale so it works regardless of the process locale.
int CharWidth(char32_t cp);

struct Style {
    int fg = -1;  // -1 = terminal default, 0–255 = palette index
    int bg = -1;
    bool bold      = false;
    bool dim       = false;
    bool italic    = false;
    bool underline = false;
    bool reverse   = false;
    std::optional<Rgb> fgRgb;  // 24-bit true color (38;2); overrides fg when set
    std::optional<Rgb> bgRgb;  // 24-bit true color (48;2); overrides bg when set

    bool operator==(const Style&) const = default;
    bool operator!=(const Style& o) const { return !(*this == o); }
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

    // Write/insert operations take the style to apply explicitly — SGR state
    // is terminal-global and owned by Document, never by a line.
    void WriteAt(size_t col, char32_t ch, const Style& style);
    void InsertAt(size_t col, char32_t ch, const Style& style);
    void DeletePreviousChar(size_t cursorCol);
    void DeleteAt(size_t col, size_t count);
    // padStyle is applied to the spaces that backfill the cleared cells.
    void DeleteAtClamped(size_t col, size_t count, size_t boundary,
                         const Style& padStyle);
    // Erase all text from col to end of line, dropping style runs at or past
    // col and truncating any run that straddles it. No-op when col is at or
    // past the end of the text.
    void TruncateFrom(size_t col);
    void Clear();
};

// Interface through which the Parser drives document mutations directly,
// eliminating the Session pass-through layer.
class IDocumentTarget {
public:
    virtual ~IDocumentTarget() = default;

    virtual void AppendInsertChar(char32_t ch) = 0;
    // Appends a contiguous run of printable characters. The parser flushes a
    // run at every control byte, so a run never spans cursor movement, line
    // breaks, or mode changes. The default loops over AppendInsertChar (one
    // lock + one notification per character); hot-path implementations
    // (MainScreenDocument) override with a single-lock, single-notification
    // batch. AltScreenDocument keeps the default: its deferred-wrap handling
    // calls NewLine() mid-run, which must not run under the lines lock.
    virtual void AppendRun(std::u32string_view run)
    {
        for (char32_t ch : run)
            AppendInsertChar(ch);
    }
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
    // SGR state is terminal-global: it survives line breaks and cursor moves,
    // exactly like a real terminal. Written and read only on the thread that
    // feeds the parser, so no lock is required.
    void SetCurrentStyle(const Style& style) override { currentStyle_ = style; }
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
    virtual void FullReset(bool clearContent) = 0;      // RIS: reset modes/attrs; if clearContent wipes all lines
    virtual void ReverseIndex() {}                       // ESC M — scroll region content down
    virtual void SetScrollRegion(int top, int bot) {}    // CSI r — set top/bottom margins (1-indexed)
    virtual void InsertLines(int count) {}               // CSI Ps L — insert blank lines at cursor
    virtual void DeleteLines(int count) {}               // CSI Ps M — delete lines at cursor
    virtual void MoveCursorToColumn(int col) {}          // CSI G — cursor column absolute (1-indexed)
    virtual void MoveCursorToRow(int row) {}             // CSI d — cursor row absolute (1-indexed)
    // PTY-configured column width (what readline was told). Used by
    // MainScreenDocument to map readline's virtual (row,col) to a flat
    // document column. Defaults are safe; Session keeps this in sync.
    // Callers must serialize this with parsing (Session holds docMutex_) —
    // cols_ is read throughout the parser-driven mutation path.
    virtual void SetPtyCols(int cols) {}
    virtual void SaveCursor() {}                         // ESC 7 / CSI s
    virtual void RestoreCursor() {}                      // ESC 8 / CSI u
    virtual void EraseChar(int count) {}                 // CSI X — erase chars at cursor, no cursor movement
    virtual void InsertChar(int count) = 0;              // CSI @ — insert blank chars, shift right
    virtual void SetInsertMode(bool on) { insertMode_ = on; }

    // Returns the index of the first line that belongs to the "current canvas"
    // (i.e. the virtual top of the display, past any scrollback).  AltScreen
    // always returns 0; MainScreenDocument returns virtualDocStartLine_.
    // DocLayout uses this to restore the viewport origin on document switch.
    virtual size_t GetScrollbackOrigin() const { return 0; }

    // Coherent cursor snapshot for cross-thread readers (UI paint, selection
    // anchors). Writers mutate cursor_ only while holding linesMutex_
    // exclusively, so a shared lock here guarantees the pair is never torn.
    CursorPos GetCursor() const
    {
        std::shared_lock<std::shared_mutex> rlk(linesMutex_);
        return cursor_;
    }
    // For callers that already hold linesMutex_ (DocLayout render path) or
    // run on the mutating thread itself (document-change notifications) —
    // taking the shared lock twice on one thread is undefined behaviour.
    CursorPos GetCursorLocked() const { return cursor_; }

    // Returned by value under titleMutex_: the title is written by the parser
    // thread (OSC 0/1/2) and read from the UI thread (tab labels).
    std::string GetTitle() const
    {
        std::lock_guard<std::mutex> lk(titleMutex_);
        return title_;
    }
    void SetTitle(const std::string& title);

    void AddListener(IDocumentListener* listener);
    void RemoveListener(IDocumentListener* listener);

    // Shared mutex protecting GetLines() content AND cursor_ in derived
    // classes.  Callers that read the deque (or need a coherent cursor) from a
    // different thread than the one that mutates it must hold a shared_lock
    // for the duration of their access.  Derived-class mutation sites must
    // hold a unique_lock that is RELEASED before any NotifyListeners call
    // (to avoid deadlock with readers that hold mtx_ first).
    std::shared_mutex& GetLinesMutex() const noexcept { return linesMutex_; }

protected:
    void NotifyListeners(DocChangeType type, size_t lineIndex);

    CursorPos   cursor_{};
    std::string title_;
    bool        insertMode_ = false;
    Style       currentStyle_{};   // accumulated SGR state; see SetCurrentStyle

    mutable std::shared_mutex linesMutex_;
    mutable std::mutex        titleMutex_;   // guards title_ (parser writes, UI reads)

private:
    std::vector<IDocumentListener*> listeners_;
    std::mutex                      listenerMutex_;
};

struct ScrollbackSnapshot {
    std::vector<DocLine> lines;
    size_t virtualDocStart = 0;
    std::string savedAt;   // ISO 8601, set at capture time by caller
};

// Builds the dim separator DocLine that marks where restored scrollback ends
// and new output begins: "--- Scrollback restored from <savedAt> ---".
// When padToCols exceeds the base text length the line is right-padded with
// dashes to that width (used to span the full terminal row); otherwise a
// plain "---" suffix closes the line.
DocLine MakeScrollbackSeparator(const std::string& savedAt, int padToCols = 0);

class MainScreenDocument : public Document {
public:
    explicit MainScreenDocument(int maxLines = 100'000);

    void AppendInsertChar(char32_t ch) override;
    void AppendRun(std::u32string_view run) override;
    void Backspace() override;
    void NewLine() override;
    void CarriageReturn() override;
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
    void FullReset(bool clearContent)          override;
    void SaveCursor()                          override;
    void RestoreCursor()                       override;

    size_t GetScrollbackOrigin()    const override { return virtualDocStartLine_; }
    void   AdvanceCanvas();
    // Resets soft terminal modes (insert mode, pending-wrap) without touching
    // content, cursor, or canvas origin.  Used by Session::ResetTerminal(false).
    void   SoftReset();

    // Thread-safe snapshot of current lines (shared_lock on linesMutex_).
    // savedAt is not populated — caller sets it before persisting.
    ScrollbackSnapshot CaptureLines() const;

    // Prepends restored lines + a styled separator before the current canvas.
    // Advances virtualDocStartLine_ and cursor_.line past the injected content.
    void LoadScrollback(const ScrollbackSnapshot& snap);

private:
    // Converts 1-indexed canvas-relative PTY row/col to a CursorPos in the
    // sub-row encoding.  Emplaces blank DocLines on demand if the target row
    // doesn't exist yet.  O(canvas rows) — bounded by terminal height.
    CursorPos PtyToDoc(int ptyRow, int ptyCol);
    std::deque<DocLine> lines_;
    int       maxLines_;
    int       cols_                = 2048;
    size_t    virtualDocStartLine_ = 0;    // origin of the current canvas within the scrollback buffer
    bool      pendingSubRowClear_      = false;
    bool      crPriorToNewLine_        = false; // set by CarriageReturn(), consumed by NewLine()
    bool      newLineWasPhantom_       = false; // \n alone past last sub-row: EraseInLine(0) pops the new DocLine
    bool      newLineCRPhantom_        = false; // \r\n emplace: MoveCursorUp pops the new DocLine if no content lands first
    CursorPos savedCursor_         = {};   // stored canvas-relative (row offset from virtualDocStartLine_)
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
    void FullReset(bool clearContent)           override;

private:
    int       rows_;
    int       cols_;
    int       scrollTop_   = 0;    // 0-indexed top margin of scroll region
    int       scrollBot_   = 0;    // 0-indexed bottom margin of scroll region
    CursorPos savedCursor_ = {};   // saved by ESC 7 / CSI s
    bool      pendingWrap_ = false; // VT100 "last column flag" — deferred autowrap
    std::deque<DocLine> lines_;    // always exactly rows_ entries
};
