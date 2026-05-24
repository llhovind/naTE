#include "document/Document.h"
#include <algorithm>
#include <stdexcept>

// Unicode East-Asian Width table (Unicode 15, TR#11).
// Ranges marked W (Wide) or F (Fullwidth) display in 2 terminal columns.
int CharWidth(char32_t cp)
{
    // clang-format off
    // Zero-width: combining / non-spacing marks
    if (cp == 0) return 0;
    if ((cp >= 0x0300 && cp <= 0x036F) ||
        (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x0591 && cp <= 0x05BD) ||
        (cp >= 0x0610 && cp <= 0x061A) ||
        (cp >= 0x064B && cp <= 0x065F) ||
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) ||
        (cp >= 0xFE20 && cp <= 0xFE2F))
        return 0;
    // Wide / Fullwidth ranges
    if ((cp >= 0x1100 && cp <= 0x115F)  ||  // Hangul Jamo
        (cp >= 0x2E80 && cp <= 0x303E)  ||  // CJK Radicals / Kangxi
        (cp >= 0x3041 && cp <= 0x33FF)  ||  // Kana, Bopomofo, CJK Compat
        (cp >= 0x3400 && cp <= 0x4DBF)  ||  // CJK Extension A
        (cp >= 0x4E00 && cp <= 0x9FFF)  ||  // CJK Unified Ideographs
        (cp >= 0xA000 && cp <= 0xA4CF)  ||  // Yi
        (cp >= 0xA960 && cp <= 0xA97F)  ||  // Hangul Jamo Extended-A
        (cp >= 0xAC00 && cp <= 0xD7A3)  ||  // Hangul Syllables
        (cp >= 0xF900 && cp <= 0xFAFF)  ||  // CJK Compat Ideographs
        (cp >= 0xFE10 && cp <= 0xFE19)  ||  // Vertical Forms
        (cp >= 0xFE30 && cp <= 0xFE6F)  ||  // CJK Compat Forms / Small Forms
        (cp >= 0xFF01 && cp <= 0xFF60)  ||  // Fullwidth Forms
        (cp >= 0xFFE0 && cp <= 0xFFE6)  ||  // Fullwidth Signs
        (cp >= 0x1B000 && cp <= 0x1B2FF)||  // Kana Extended
        (cp >= 0x1F004 && cp <= 0x1F004)||  // Mahjong tile
        (cp >= 0x1F0CF && cp <= 0x1F0CF)||  // Playing card
        (cp >= 0x1F200 && cp <= 0x1F2FF)||  // Enclosed CJK
        (cp >= 0x1F300 && cp <= 0x1F64F)||  // Misc Symbols / Emoticons
        (cp >= 0x1F900 && cp <= 0x1F9FF)||  // Supplemental Symbols
        (cp >= 0x20000 && cp <= 0x2FFFD)||  // CJK Extension B–F
        (cp >= 0x30000 && cp <= 0x3FFFD))   // CJK Extension G+
        return 2;
    // clang-format on
    return 1;
}

// ---------------------------------------------------------------------------
// DocLine
// ---------------------------------------------------------------------------

void DocLine::WriteAt(size_t col, char32_t ch)
{
    // --- pad: cursor jumped past end of line ---
    if (col > text.size()) {
        const size_t padStart = text.size();
        const size_t padCount = col - padStart;
        text.append(padCount, U' ');
        if (!styles.empty() && styles.back().style == Style{} &&
            styles.back().start + styles.back().length == padStart)
            styles.back().length += padCount;
        else
            styles.push_back({padStart, padCount, Style{}});
    }

    // --- append: cursor is at end of line ---
    if (col == text.size()) {
        text.push_back(ch);
        if (!styles.empty() && styles.back().style == currentStyle &&
            styles.back().start + styles.back().length == col)
            ++styles.back().length;
        else
            styles.push_back({col, 1, currentStyle});
        return;
    }

    // --- overwrite: cursor is within existing text ---
    text[col] = ch;
    for (size_t i = 0; i < styles.size(); ++i) {
        const size_t rStart = styles[i].start;
        const size_t rEnd   = rStart + styles[i].length;
        if (rStart > col || rEnd <= col) continue;

        if (styles[i].style == currentStyle) return;

        const Style  old       = styles[i].style;
        const size_t beforeLen = col - rStart;
        const size_t afterLen  = rEnd - col - 1;

        styles.erase(styles.begin() + i);

        size_t at = i;
        if (beforeLen > 0) styles.insert(styles.begin() + at++, {rStart,   beforeLen, old});
        styles.insert(styles.begin() + at++,                    {col,       1,         currentStyle});
        if (afterLen  > 0) styles.insert(styles.begin() + at,   {col + 1,  afterLen,  old});
        return;
    }
    // col not covered by any run (gap in style tracking)
    styles.push_back({col, 1, currentStyle});
}

void DocLine::Clear()
{
    text.clear();
    styles.clear();
}

void DocLine::DeletePreviousChar(size_t cursorCol)
{
    if (cursorCol == 0 || text.empty())
        return;

    const size_t erasePos = cursorCol - 1;
    if (erasePos >= text.size())
        return;

    text.erase(erasePos, 1);

    for (auto it = styles.begin(); it != styles.end(); ) {
        const size_t runEnd = it->start + it->length;
        if (it->start > erasePos) {
            --it->start;
            ++it;
        } else if (runEnd > erasePos) {
            --it->length;
            it = (it->length == 0) ? styles.erase(it) : ++it;
        } else {
            ++it;
        }
    }
}

// Erase `count` characters starting at `col`, shifting the rest of the line
// left. Adjusts style runs accordingly. Clamps to the available text.
void DocLine::DeleteAt(size_t col, size_t count)
{
    if (col >= text.size() || count == 0)
        return;
    count = std::min(count, text.size() - col);
    text.erase(col, count);

    for (auto it = styles.begin(); it != styles.end(); ) {
        const size_t runEnd = it->start + it->length;

        if (it->start >= col + count) {
            // Run is entirely after the deleted range — shift left.
            it->start -= count;
            ++it;
        } else if (runEnd <= col) {
            // Run is entirely before the deleted range — unchanged.
            ++it;
        } else {
            // Run overlaps the deleted range; shrink it.
            const size_t overlapStart  = std::max(it->start, col);
            const size_t overlapEnd    = std::min(runEnd, col + count);
            const size_t deleted       = overlapEnd - overlapStart;
            it->start = std::min(it->start, col);
            it->length -= deleted;
            it = (it->length == 0) ? styles.erase(it) : ++it;
        }
    }
}

// VT100 CSI P semantics bounded to a visual row [col, boundary).
// Deletes eff=min(count, boundary-col) chars at col, shifts [col+eff, boundary) left,
// fills [boundary-eff, boundary) with spaces, leaves [boundary, ...) untouched.
// Style-safe: composed entirely from DeleteAt and InsertAt.
void DocLine::DeleteAtClamped(size_t col, size_t count, size_t boundary)
{
    if (col >= text.size() || col >= boundary || count == 0)
        return;

    const size_t eff = std::min(count, boundary - col);

    if (text.size() <= boundary) {
        // No content past boundary — plain bounded delete, nothing to protect.
        DeleteAt(col, std::min(eff, text.size() - col));
        return;
    }

    // Content exists at [boundary, ...). DeleteAt shifts it left by eff into
    // [boundary-eff, ...). Re-inserting eff spaces at boundary-eff pushes the
    // suffix back to [boundary, ...) and fills the gap with spaces.
    DeleteAt(col, eff);
    for (size_t i = 0; i < eff; ++i)
        InsertAt(boundary - eff, U' ');
}

// Insert `ch` at `col`, shifting all characters from `col` rightward by one.
// If `col` is past the end of the line it delegates to WriteAt (pad + append).
void DocLine::InsertAt(size_t col, char32_t ch)
{
    if (col >= text.size()) {
        WriteAt(col, ch);
        return;
    }

    text.insert(col, 1, ch);

    for (size_t i = 0; i < styles.size(); ++i) {
        const size_t rStart = styles[i].start;
        const size_t rEnd   = rStart + styles[i].length;

        if (rStart >= col) {
            // Entirely at or after insertion point — shift right.
            ++styles[i].start;
        } else if (rEnd > col) {
            // Run straddles col: split into before / new-char / after.
            const Style  old       = styles[i].style;
            const size_t beforeLen = col - rStart;
            const size_t afterLen  = rEnd - col;   // chars originally at [col, rEnd)

            styles[i].length = beforeLen;           // shrink to "before" portion
            // Insert new-char run and shifted "after" run after current index.
            styles.insert(styles.begin() + static_cast<ptrdiff_t>(i) + 1,
                          {col + 1, afterLen, old});
            styles.insert(styles.begin() + static_cast<ptrdiff_t>(i) + 1,
                          {col,     1,        currentStyle});
            return;
        }
        // else: run is entirely before col — no change.
    }
    // Insertion point not covered by any run — add a new single-cell run.
    styles.push_back({col, 1, currentStyle});
}

// ---------------------------------------------------------------------------
// Document — listener management
// ---------------------------------------------------------------------------

void Document::SetTitle(const std::string& title)
{
    title_ = title;
    NotifyListeners(DocChangeType::TitleChanged, 0);
}

void Document::AddListener(IDocumentListener* l)
{
    std::lock_guard lock(listenerMutex_);
    listeners_.push_back(l);
}

void Document::RemoveListener(IDocumentListener* l)
{
    std::lock_guard lock(listenerMutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}

void Document::NotifyListeners(DocChangeType type, size_t lineIndex)
{
    std::lock_guard lock(listenerMutex_);
    for (auto* l : listeners_)
        l->OnDocumentChanged(type, lineIndex);
}

// ---------------------------------------------------------------------------
// MainScreenDocument
// ---------------------------------------------------------------------------

MainScreenDocument::MainScreenDocument(int maxLines)
    : maxLines_(maxLines)
{
    lines_.emplace_back();
    cursor_ = {0, 0};
}

void MainScreenDocument::AppendInsertChar(char32_t ch)
{
    const int w = CharWidth(ch);
    if (insertMode_) {
        lines_.back().InsertAt(cursor_.col, ch);
        if (w == 2) lines_.back().InsertAt(cursor_.col + 1, kWideFiller);
    } else {
        lines_.back().WriteAt(cursor_.col, ch);
        if (w == 2) lines_.back().WriteAt(cursor_.col + 1, kWideFiller);
    }
    cursor_.col += static_cast<size_t>(w);
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::Backspace()
{
    DocLine& line = lines_.back();
    if (cursor_.col == 0 || line.text.empty())
        return;

    line.DeletePreviousChar(cursor_.col);
    --cursor_.col;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::NewLine()
{
    const size_t lineLen    = lines_[cursor_.line].text.size();
    const int    lastSubRow = lineLen == 0 ? 0 : (int)((lineLen - 1) / (size_t)cols_);
    const int    cursorSubRow = (int)(cursor_.col / (size_t)cols_);

    if (cursorSubRow < lastSubRow) {
        cursor_.col = static_cast<size_t>(cursorSubRow + 1) * static_cast<size_t>(cols_);
        NotifyListeners(DocChangeType::CursorMove, cursor_.line);
        return;
    }

    lines_.emplace_back();
    ++cursor_.line;
    cursor_.col = 0;
    NotifyListeners(DocChangeType::InsertLine, cursor_.line);

    if ((int)lines_.size() > maxLines_) {
        lines_.pop_front();
        --cursor_.line;
        if (virtualDocStartLine_ > 0) --virtualDocStartLine_;
        // savedCursor_ is canvas-relative — no adjustment needed.
        NotifyListeners(DocChangeType::DeleteLine, 0);
    }
}

void MainScreenDocument::CarriageReturn()
{
    const size_t c = static_cast<size_t>(cols_);
    cursor_.col = (cursor_.col / c) * c;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::SetCurrentStyle(const Style& style)
{
    lines_.back().currentStyle = style;
}

void MainScreenDocument::MoveCursorLeft(int n)
{
    cursor_.col = (cursor_.col >= static_cast<size_t>(n))
                    ? cursor_.col - static_cast<size_t>(n)
                    : 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorRight(int n)
{
    cursor_.col += static_cast<size_t>(n);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::SetPtyCols(int cols)
{
    cols_ = std::max(1, cols);
}

void MainScreenDocument::SaveCursor()
{
    // Store canvas-relative row so restore survives a canvas advance.
    savedCursor_ = { cursor_.line - virtualDocStartLine_, cursor_.col };
}

void MainScreenDocument::RestoreCursor()
{
    const size_t target = virtualDocStartLine_ + savedCursor_.line;
    while (lines_.size() <= target)
        lines_.emplace_back();
    cursor_ = { target, savedCursor_.col };
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorUp(int n)
{
    const size_t delta = static_cast<size_t>(n) * static_cast<size_t>(cols_);
    cursor_.col = (cursor_.col >= delta) ? cursor_.col - delta : 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorDown(int n)
{
    cursor_.col += static_cast<size_t>(n) * static_cast<size_t>(cols_);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::EraseInLine(int mode)
{
    DocLine& line = lines_.back();
    switch (mode) {
    case 0: { // cursor to end of line
        const bool hadPendingClear = pendingSubRowClear_;
        pendingSubRowClear_ = false;
        // Phantom line: NewLine() appended a DocLine when readline navigated
        // to what it believed was a second visual row, but the preceding
        // EraseInLine had already erased that row's content. Remove the
        // phantom and restore cursor to the end of the now-current line.
        if (cursor_.col == 0 && line.text.empty() && cursor_.line > 0) {
            lines_.pop_back();
            --cursor_.line;
            cursor_.col = lines_[cursor_.line].text.size();
            NotifyListeners(DocChangeType::DeleteLine, cursor_.line + 1);
            return;
        }
        if (cursor_.col < line.text.size()) {
            line.text.erase(cursor_.col);
            for (auto it = line.styles.begin(); it != line.styles.end(); ) {
                if (it->start >= cursor_.col) {
                    it = line.styles.erase(it);
                } else if (it->start + it->length > cursor_.col) {
                    it->length = cursor_.col - it->start;
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        // After erasing subRow 1+ content (cursor is exactly at a subRow
        // start), trim trailing spaces left by DeleteAtClamped padding and
        // reset cursor to the true content end. Gated on hadPendingClear to
        // avoid false-positive trims from unrelated erases at a subRow boundary.
        if (hadPendingClear && cursor_.col > 0
                            && cursor_.col % static_cast<size_t>(cols_) == 0) {
            while (!line.text.empty() && line.text.back() == U' ') {
                line.text.pop_back();
                for (auto it = line.styles.begin(); it != line.styles.end(); ) {
                    const size_t newLen = line.text.size();
                    if (it->start >= newLen)
                        it = line.styles.erase(it);
                    else if (it->start + it->length > newLen)
                        { it->length = newLen - it->start; ++it; }
                    else
                        ++it;
                }
            }
            cursor_.col = line.text.size();
        }
        break;
    }
    case 1: // beginning of line to cursor (inclusive) — fill with spaces
        for (size_t i = 0; i <= cursor_.col && i < line.text.size(); ++i)
            line.text[i] = U' ';
        break;
    case 2: // entire line
        line.Clear();
        break;
    }
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::MoveCursorToColumn(int col)
{
    const size_t c        = static_cast<size_t>(cols_);
    const size_t virtRow  = cursor_.col / c;
    cursor_.col = virtRow * c + static_cast<size_t>(std::max(1, col) - 1);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorToLineStart()
{
    cursor_.col = 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorToLineEnd()
{
    cursor_.col = lines_[cursor_.line].text.size();
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

// row and col are 1-indexed (VT100 convention).
// Row is canvas-relative: row 1 == virtualDocStartLine_.
// Blank lines are appended on demand if the target row doesn't exist yet.
void MainScreenDocument::MoveCursorToPosition(int row, int col)
{
    const size_t target = virtualDocStartLine_ + static_cast<size_t>(std::max(1, row) - 1);
    while (lines_.size() <= target)
        lines_.emplace_back();
    cursor_.line = target;
    cursor_.col  = static_cast<size_t>(std::max(1, col) - 1);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

// CSI d — canvas-relative row, same semantics as MoveCursorToPosition.
void MainScreenDocument::MoveCursorToRow(int row)
{
    const size_t target = virtualDocStartLine_ + static_cast<size_t>(std::max(1, row) - 1);
    while (lines_.size() <= target)
        lines_.emplace_back();
    cursor_.line = target;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::EraseChar(int count)
{
    const size_t end = std::min(cursor_.col + static_cast<size_t>(count),
                                lines_[cursor_.line].text.size());
    for (size_t i = cursor_.col; i < end; ++i)
        lines_[cursor_.line].WriteAt(i, U' ');
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::DeleteChar(int count)
{
    const size_t c       = static_cast<size_t>(cols_);
    const size_t lineLen = lines_[cursor_.line].text.size();

    if (lineLen > c) {
        // Multi-subRow line: scope deletion to the current visual row so content
        // on subRow N+1 is never shifted into subRow N (VT100 CSI P invariant).
        // The condition is lineLen > c ("any multi-subRow line"), not
        // lineLen > subRowEnd, to also handle cursor positions in subRow 1+.
        const size_t subRowEnd = (cursor_.col / c + 1) * c;
        lines_[cursor_.line].DeleteAtClamped(cursor_.col,
                                             static_cast<size_t>(count),
                                             subRowEnd);
        pendingSubRowClear_ = true;
    } else {
        lines_[cursor_.line].DeleteAt(cursor_.col, static_cast<size_t>(count));
    }
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::InsertChar(int count)
{
    for (int i = 0; i < count; ++i)
        lines_[cursor_.line].InsertAt(cursor_.col, U' ');
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::InsertLines(int count)
{
    for (int i = 0; i < count; ++i) {
        lines_.insert(lines_.begin() + cursor_.line, DocLine{});
        NotifyListeners(DocChangeType::InsertLine, cursor_.line);
    }
}

void MainScreenDocument::DeleteLines(int count)
{
    const int n = (int)lines_.size();
    count = std::min(count, n - (int)cursor_.line);
    for (int i = 0; i < count; ++i) {
        lines_.erase(lines_.begin() + cursor_.line);
        NotifyListeners(DocChangeType::DeleteLine, cursor_.line);
    }
}

void MainScreenDocument::AdvanceCanvas()
{
    virtualDocStartLine_ = lines_.size();
    lines_.emplace_back();
    cursor_ = { virtualDocStartLine_, 0 };
    // CanvasReset carries the new cursor position implicitly; no separate CursorMove
    // needed — firing one would trigger ScrollToEndLocked and override the anchor.
    NotifyListeners(DocChangeType::CanvasReset, virtualDocStartLine_);
}

void MainScreenDocument::SoftReset()
{
    insertMode_         = false;
    pendingSubRowClear_ = false;
    // Scroll the viewport to show the current cursor so the user can see where
    // output will land after the remote re-initialises.
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::EraseInDisplay(int mode)
{
    switch (mode) {
    case 0: { // erase from cursor to end of canvas
        if (cursor_.line == virtualDocStartLine_ && cursor_.col == 0) {
            // Cursor at canvas top-left — server wants a new canvas.
            AdvanceCanvas();
            break;
        }
        // Mid-canvas: blank cursor line from cursor col, then blank lines below.
        // Lines are kept (not popped) to preserve document structure.
        DocLine& cur = lines_[cursor_.line];
        if (cursor_.col < cur.text.size()) {
            cur.text.erase(cursor_.col);
            for (auto it = cur.styles.begin(); it != cur.styles.end(); ) {
                if (it->start >= cursor_.col)
                    it = cur.styles.erase(it);
                else if (it->start + it->length > cursor_.col) {
                    it->length = cursor_.col - it->start;
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
        for (size_t i = cursor_.line + 1; i < lines_.size(); ++i) {
            lines_[i].Clear();
            NotifyListeners(DocChangeType::UpdateLine, i);
        }
        break;
    }
    case 1: { // erase from beginning of canvas to cursor
        for (size_t i = virtualDocStartLine_; i < cursor_.line; ++i) {
            lines_[i].Clear();
            NotifyListeners(DocChangeType::UpdateLine, i);
        }
        DocLine& cur = lines_[cursor_.line];
        for (size_t i = 0; i <= cursor_.col && i < cur.text.size(); ++i)
            cur.text[i] = U' ';
        NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
        break;
    }
    case 2: // fall-through
    case 3: { // server wants a new canvas (mode 3 also clears scrollback in xterm, ignored here)
        AdvanceCanvas();
        break;
    }
    }
}

void MainScreenDocument::FullReset(bool clearContent)
{
    insertMode_         = false;
    pendingSubRowClear_ = false;
    title_.clear();

    if (clearContent) {
        lines_.clear();
        lines_.emplace_back();
        cursor_              = {0, 0};
        virtualDocStartLine_ = 0;
        NotifyListeners(DocChangeType::CanvasReset, 0);
    } else {
        AdvanceCanvas();
    }
}

// ---------------------------------------------------------------------------
// AltScreenDocument — fixed-size, fully-addressable grid
// ---------------------------------------------------------------------------

AltScreenDocument::AltScreenDocument(int rows, int cols)
    : rows_(rows), cols_(cols), scrollTop_(0), scrollBot_(rows - 1)
{
    lines_.resize(static_cast<size_t>(rows));
    cursor_ = {0, 0};
}

void AltScreenDocument::Resize(int rows, int cols)
{
    // Notify removal of all lines beyond the new row count (top-down for DocLayout).
    const int oldRows = static_cast<int>(lines_.size());
    for (int i = oldRows - 1; i >= rows; --i)
        NotifyListeners(DocChangeType::DeleteLine, static_cast<size_t>(i));

    lines_.clear();
    lines_.resize(static_cast<size_t>(rows));
    rows_ = rows;
    cols_ = cols;
    scrollTop_ = 0;
    scrollBot_ = rows_ - 1;
    pendingWrap_ = false;

    cursor_.line = std::min(cursor_.line, static_cast<size_t>(rows_ - 1));
    cursor_.col  = std::min(cursor_.col,  static_cast<size_t>(cols_ - 1));

    // Notify all remaining lines as updated so DocLayout rebuilds metadata.
    for (int i = 0; i < rows_; ++i)
        NotifyListeners(DocChangeType::UpdateLine, static_cast<size_t>(i));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::AppendInsertChar(char32_t ch)
{
    if (pendingWrap_) {
        pendingWrap_ = false;
        NewLine();
    }

    const int    w      = CharWidth(ch);
    const size_t newCol = cursor_.col + static_cast<size_t>(w);

    if (insertMode_) {
        lines_[cursor_.line].InsertAt(cursor_.col, ch);
        if (w == 2 && cursor_.col + 1 < static_cast<size_t>(cols_))
            lines_[cursor_.line].InsertAt(cursor_.col + 1, kWideFiller);
    } else {
        lines_[cursor_.line].WriteAt(cursor_.col, ch);
        if (w == 2 && cursor_.col + 1 < static_cast<size_t>(cols_))
            lines_[cursor_.line].WriteAt(cursor_.col + 1, kWideFiller);
    }
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);

    if (newCol >= static_cast<size_t>(cols_))
        pendingWrap_ = true;  // wrap deferred until next printable char
    else
        cursor_.col = newCol;
}

void AltScreenDocument::Backspace()
{
    pendingWrap_ = false;
    if (cursor_.col == 0) return;
    --cursor_.col;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void AltScreenDocument::NewLine()
{
    pendingWrap_ = false;
    const size_t bot = static_cast<size_t>(scrollBot_);
    const size_t top = static_cast<size_t>(scrollTop_);

    if (cursor_.line < bot) {
        ++cursor_.line;
        cursor_.col = 0;
        NotifyListeners(DocChangeType::CursorMove, cursor_.line);
    } else if (cursor_.line == bot) {
        // At the bottom of the scroll region: scroll up within the region.
        lines_.erase(lines_.begin() + scrollTop_);
        NotifyListeners(DocChangeType::DeleteLine, top);
        lines_.insert(lines_.begin() + scrollBot_, DocLine{});
        NotifyListeners(DocChangeType::InsertLine, bot);
        cursor_.col = 0;
        // cursor_.line stays at scrollBot_
    } else {
        // Below scroll region — just advance without scrolling.
        cursor_.line = std::min(cursor_.line + 1, static_cast<size_t>(rows_ - 1));
        cursor_.col = 0;
        NotifyListeners(DocChangeType::CursorMove, cursor_.line);
    }
}

void AltScreenDocument::SetScrollRegion(int top, int bot)
{
    // top and bot are 1-indexed; 0 means "use default" (full screen).
    scrollTop_ = (top <= 0) ? 0 : top - 1;
    scrollBot_ = (bot <= 0 || bot > rows_) ? rows_ - 1 : bot - 1;
    if (scrollTop_ >= rows_) scrollTop_ = 0;
    if (scrollBot_ < scrollTop_) scrollBot_ = rows_ - 1;
    // VT100 spec: cursor moves to home on DECSTBM.
    pendingWrap_ = false;
    cursor_ = {static_cast<size_t>(scrollTop_), 0};
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::ReverseIndex()
{
    const size_t top = static_cast<size_t>(scrollTop_);
    const size_t bot = static_cast<size_t>(scrollBot_);

    if (cursor_.line > top) {
        --cursor_.line;
        NotifyListeners(DocChangeType::CursorMove, cursor_.line);
    } else {
        // Cursor is at the top of the scroll region — scroll content DOWN.
        // Insert a blank line at scrollTop_, remove the line below scrollBot_.
        lines_.insert(lines_.begin() + scrollTop_, DocLine{});
        NotifyListeners(DocChangeType::InsertLine, top);
        lines_.erase(lines_.begin() + scrollBot_ + 1);
        NotifyListeners(DocChangeType::DeleteLine, bot + 1);
    }
}

void AltScreenDocument::MoveCursorToColumn(int col)
{
    pendingWrap_ = false;
    cursor_.col = std::min(static_cast<size_t>(std::max(1, col) - 1),
                           static_cast<size_t>(cols_ - 1));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorToRow(int row)
{
    pendingWrap_ = false;
    cursor_.line = std::min(static_cast<size_t>(std::max(1, row) - 1),
                            static_cast<size_t>(rows_ - 1));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::SaveCursor()
{
    savedCursor_ = cursor_;
}

void AltScreenDocument::RestoreCursor()
{
    pendingWrap_ = false;
    cursor_.line = std::min(savedCursor_.line, static_cast<size_t>(rows_ - 1));
    cursor_.col  = std::min(savedCursor_.col,  static_cast<size_t>(cols_ - 1));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::CarriageReturn()
{
    pendingWrap_ = false;
    cursor_.col = 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::SetCurrentStyle(const Style& style)
{
    lines_[cursor_.line].currentStyle = style;
}

void AltScreenDocument::MoveCursorLeft(int n)
{
    pendingWrap_ = false;
    cursor_.col = (cursor_.col >= static_cast<size_t>(n))
                    ? cursor_.col - static_cast<size_t>(n)
                    : 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorRight(int n)
{
    pendingWrap_ = false;
    cursor_.col = std::min(cursor_.col + static_cast<size_t>(n),
                           static_cast<size_t>(cols_ - 1));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorUp(int n)
{
    pendingWrap_ = false;
    cursor_.line = (cursor_.line >= static_cast<size_t>(n))
                     ? cursor_.line - static_cast<size_t>(n)
                     : 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorDown(int n)
{
    pendingWrap_ = false;
    cursor_.line = std::min(cursor_.line + static_cast<size_t>(n),
                            static_cast<size_t>(rows_ - 1));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorToLineStart()
{
    pendingWrap_ = false;
    cursor_.col = 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorToLineEnd()
{
    pendingWrap_ = false;
    cursor_.col = static_cast<size_t>(cols_ - 1);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::MoveCursorToPosition(int row, int col)
{
    pendingWrap_ = false;
    const size_t r = static_cast<size_t>(std::max(1, row) - 1);
    const size_t c = static_cast<size_t>(std::max(1, col) - 1);
    cursor_.line = std::min(r, static_cast<size_t>(rows_ - 1));
    cursor_.col  = std::min(c, static_cast<size_t>(cols_ - 1));
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void AltScreenDocument::EraseInLine(int mode)
{
    DocLine& line = lines_[cursor_.line];
    switch (mode) {
    case 0: // cursor to end of line
        if (cursor_.col < line.text.size()) {
            line.text.erase(cursor_.col);
            for (auto it = line.styles.begin(); it != line.styles.end(); ) {
                if (it->start >= cursor_.col)
                    it = line.styles.erase(it);
                else if (it->start + it->length > cursor_.col) {
                    it->length = cursor_.col - it->start;
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        break;
    case 1: // beginning of line to cursor (inclusive) — fill with spaces
        for (size_t i = 0; i <= cursor_.col && i < line.text.size(); ++i)
            line.text[i] = U' ';
        break;
    case 2: // entire line
        line.Clear();
        break;
    }
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void AltScreenDocument::EraseChar(int count)
{
    // Fill `count` cells from cursor with spaces using the current style.
    // Cursor does not move.
    const size_t end = std::min(cursor_.col + static_cast<size_t>(count),
                                static_cast<size_t>(cols_));
    for (size_t i = cursor_.col; i < end; ++i)
        lines_[cursor_.line].WriteAt(i, U' ');
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void AltScreenDocument::DeleteChar(int count)
{
    lines_[cursor_.line].DeleteAt(cursor_.col, static_cast<size_t>(count));
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void AltScreenDocument::InsertChar(int count)
{
    DocLine& line = lines_[cursor_.line];
    for (int i = 0; i < count; ++i)
        line.InsertAt(cursor_.col, U' ');
    // Clamp line to cols_ — characters pushed past the right margin are lost.
    if (line.text.size() > static_cast<size_t>(cols_)) {
        line.DeleteAt(static_cast<size_t>(cols_),
                      line.text.size() - static_cast<size_t>(cols_));
    }
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void AltScreenDocument::InsertLines(int count)
{
    // CSI Ps L — ignored when cursor is outside the scroll region.
    if ((int)cursor_.line < scrollTop_ || (int)cursor_.line > scrollBot_) return;
    count = std::min(count, scrollBot_ - (int)cursor_.line + 1);
    if (count == 0) return;

    const int cur = (int)cursor_.line;
    const int bot = scrollBot_;

    // Shift lines within [cur, bot] down by count; lines at the bottom are lost.
    for (int i = bot; i >= cur + count; --i)
        lines_[i] = lines_[i - count];
    for (int i = cur; i < cur + count; ++i)
        lines_[i] = DocLine{};

    for (int r = cur; r <= bot; ++r)
        NotifyListeners(DocChangeType::UpdateLine, static_cast<size_t>(r));
}

void AltScreenDocument::DeleteLines(int count)
{
    // CSI Ps M — ignored when cursor is outside the scroll region.
    if ((int)cursor_.line < scrollTop_ || (int)cursor_.line > scrollBot_) return;
    count = std::min(count, scrollBot_ - (int)cursor_.line + 1);
    if (count == 0) return;

    const int cur = (int)cursor_.line;
    const int bot = scrollBot_;

    // Shift lines within [cur, bot] up by count; blank lines fill the bottom.
    for (int i = cur; i + count <= bot; ++i)
        lines_[i] = lines_[i + count];
    for (int i = std::max(cur, bot - count + 1); i <= bot; ++i)
        lines_[i] = DocLine{};

    for (int r = cur; r <= bot; ++r)
        NotifyListeners(DocChangeType::UpdateLine, static_cast<size_t>(r));
}

void AltScreenDocument::EraseInDisplay(int mode)
{
    switch (mode) {
    case 0: { // cursor to end of display
        DocLine& cur = lines_[cursor_.line];
        if (cursor_.col < cur.text.size()) {
            cur.text.erase(cursor_.col);
            for (auto it = cur.styles.begin(); it != cur.styles.end(); ) {
                if (it->start >= cursor_.col)
                    it = cur.styles.erase(it);
                else if (it->start + it->length > cursor_.col) {
                    it->length = cursor_.col - it->start;
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
        for (size_t i = cursor_.line + 1; i < lines_.size(); ++i) {
            lines_[i].Clear();
            NotifyListeners(DocChangeType::UpdateLine, i);
        }
        break;
    }
    case 1: { // beginning of display to cursor
        for (size_t i = 0; i < cursor_.line; ++i) {
            lines_[i].Clear();
            NotifyListeners(DocChangeType::UpdateLine, i);
        }
        DocLine& cur = lines_[cursor_.line];
        for (size_t i = 0; i <= cursor_.col && i < cur.text.size(); ++i)
            cur.text[i] = U' ';
        NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
        break;
    }
    case 2: // fall-through
    case 3: { // erase entire display — grid stays fixed-size
        for (size_t i = 0; i < lines_.size(); ++i) {
            lines_[i].Clear();
            NotifyListeners(DocChangeType::UpdateLine, i);
        }
        pendingWrap_ = false;
        cursor_ = {0, 0};
        NotifyListeners(DocChangeType::CursorMove, 0);
        break;
    }
    }
}

void AltScreenDocument::FullReset(bool /*clearContent*/)
{
    scrollTop_   = 0;
    scrollBot_   = rows_ - 1;
    savedCursor_ = {};
    insertMode_  = false;
    pendingWrap_ = false;
    title_.clear();
    cursor_      = {0, 0};
    const int n  = static_cast<int>(lines_.size());
    for (int i = 0; i < n; ++i) {
        lines_[i].Clear();
        NotifyListeners(DocChangeType::UpdateLine, i);
    }
    NotifyListeners(DocChangeType::CursorMove, 0);
}
