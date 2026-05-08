#include "ui/DocLayout.h"
#include "ui/SearchMatch.h"
#include <algorithm>
#include <limits>

static char32_t CaseFold(char32_t c)
{
    return (c >= U'A' && c <= U'Z') ? c + (U'a' - U'A') : c;
}

DocLayout::DocLayout(Document& doc, int cols, int rows)
    : doc_(&doc), cols_(cols), rows_(rows)
{
    doc_->AddListener(this);
    ScrollToEndLocked();
}

DocLayout::~DocLayout()
{
    doc_->RemoveListener(this);
}

void DocLayout::SetDocument(Document& newDoc)
{
    std::lock_guard<std::mutex> lk(mtx_);
    doc_->RemoveListener(this);
    doc_ = &newDoc;
    doc_->AddListener(this);
    topAnchor_  = {0, 0};
    autoScroll_ = true;
    ComputeMaxVisibleWidthLocked();
}

// ---------------------------------------------------------------------------
// Core helpers — lock-free, called with mtx_ held.
// ---------------------------------------------------------------------------

// Visual sub-row count for one document line: 1 when wrapping is off or the
// line is empty; ceil(len / cols_) otherwise.
int DocLayout::VisualCount(const DocLine& line) const
{
    if (!wordWrap_ || cols_ <= 0) return 1;
    const int len = (int)line.text.size();
    return len == 0 ? 1 : (len + cols_ - 1) / cols_;
}

// Walk the viewport anchor forward (delta > 0) or backward (delta < 0) by
// exactly |delta| visual rows, crossing document-line boundaries as needed.
// Clamps at the document start/end — never returns a docLine outside [0, N).
DocLayout::ViewportAnchor
DocLayout::WalkAnchorBy(ViewportAnchor a, int delta) const
{
    const auto& lines = doc_->GetLines();

    while (delta > 0 && a.docLine < (int)lines.size()) {
        const int remaining = VisualCount(lines[a.docLine]) - a.subRow - 1;
        if (delta <= remaining) { a.subRow += delta; return a; }
        delta -= remaining + 1;
        ++a.docLine;
        a.subRow = 0;
    }

    while (delta < 0) {
        if (a.subRow >= -delta) { a.subRow += delta; return a; }
        delta += a.subRow + 1;
        if (a.docLine == 0) { a.subRow = 0; return a; }
        --a.docLine;
        a.subRow = VisualCount(lines[a.docLine]) - 1;
    }

    return a;
}

// ---------------------------------------------------------------------------
// Public API — each acquires mtx_ then delegates to the *Locked internals.
// ---------------------------------------------------------------------------

void DocLayout::SetViewportSize(int newCols, int newRows)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const int prevCols = cols_;
    cols_ = newCols;
    rows_ = newRows;

    // After a column-width change the anchor's subRow may be out of range for
    // the new wrap geometry — clamp it before any further navigation.
    if (wordWrap_) {
        const auto& lines = doc_->GetLines();
        if (topAnchor_.docLine < (int)lines.size()) {
            const int maxSub = VisualCount(lines[topAnchor_.docLine]) - 1;
            topAnchor_.subRow = std::min(topAnchor_.subRow, maxSub);
        }
    } else if (newCols > prevCols) {
        // Viewport grew: leftCol_ may have been pushed right by an earlier
        // undersized viewport (e.g. before panel layout completes on startup).
        // Reset to the leftmost position that still keeps the cursor visible.
        const int cursorCol = (int)doc_->GetCursor().col;
        leftCol_ = std::max(0, cursorCol - cols_ + 1);
    }

    if (autoScroll_) ScrollToEndLocked();
    ComputeMaxVisibleWidthLocked();
}

// r is viewport-relative: 0 = topmost visible line.
RenderedLine DocLayout::GetRenderedLine(int r)
{
    std::lock_guard<std::mutex> lk(mtx_);

    const auto&    lines = doc_->GetLines();
    const ViewportAnchor pos = WalkAnchorBy(topAnchor_, r);

    if (pos.docLine >= (int)lines.size())
        return {};  // empty row — past end of document

    const DocLine& dline    = lines[pos.docLine];
    const size_t   startCol = wordWrap_
                            ? static_cast<size_t>(pos.subRow) * static_cast<size_t>(cols_)
                            : static_cast<size_t>(leftCol_);
    const size_t   len      = (dline.text.size() > startCol)
                            ? std::min(static_cast<size_t>(cols_), dline.text.size() - startCol)
                            : 0;

    RenderedLine result;
    if (len > 0)
        result.text = dline.text.substr(startCol, len);
    result.attrs.assign(len, Style{});

    // Expand StyleRuns into a flat per-character array for the visible window
    // [startCol, startCol+len) only.
    for (const auto& sr : dline.styles) {
        const size_t srEnd    = sr.start + sr.length;
        const size_t sliceEnd = startCol + len;
        if (sr.start >= sliceEnd || srEnd <= startCol) continue;
        const size_t overlapStart = std::max(sr.start, startCol);
        const size_t overlapEnd   = std::min(srEnd, sliceEnd);
        for (size_t c = overlapStart; c < overlapEnd; ++c)
            result.attrs[c - startCol] = sr.style;
    }

    // Search highlight overlay: runs over the base attrs[], replacing bg/fg for matches.
    for (size_t mi = 0; mi < searchMatches_.size(); ++mi) {
        const SearchMatch& m = searchMatches_[mi];
        if (m.lineIndex != (size_t)pos.docLine) continue;
        const size_t mEnd = m.colStart + m.colLen;
        const size_t sliceEnd = startCol + len;
        if (mEnd <= startCol || m.colStart >= sliceEnd) continue;
        const size_t overlapStart = std::max(m.colStart, startCol);
        const size_t overlapEnd   = std::min(mEnd, sliceEnd);
        const bool isCurrent = (mi == searchCurrentIdx_);
        Style hl;
        hl.bg = isCurrent ? SearchHighlight::kCurrentBg : SearchHighlight::kMatchBg;
        hl.fg = SearchHighlight::kFg;
        for (size_t c = overlapStart; c < overlapEnd; ++c)
            result.attrs[c - startCol] = hl;
    }

    // Selection highlight overlay — drawn on top of search highlights.
    if (selection_.active) {
        auto [selStart, selEnd] = NormalizeSelectionLocked();
        if (selStart.docLine <= pos.docLine && pos.docLine <= selEnd.docLine) {
            const size_t from = (pos.docLine == selStart.docLine)
                              ? static_cast<size_t>(selStart.docCol) : 0;
            const size_t to   = (pos.docLine == selEnd.docLine)
                              ? static_cast<size_t>(selEnd.docCol)
                              : dline.text.size();
            const size_t sliceEnd     = startCol + len;
            const size_t overlapStart = std::max(from, startCol);
            const size_t overlapEnd   = std::min(to, sliceEnd);
            if (overlapStart < overlapEnd) {
                Style selStyle;
                selStyle.fg = SelectionHighlight::kFg;
                selStyle.bg = SelectionHighlight::kBg;
                for (size_t c = overlapStart; c < overlapEnd; ++c)
                    result.attrs[c - startCol] = selStyle;
            }
        }
    }

    // Cursor: check whether the document cursor lands on this exact visual row.
    const CursorPos cursor = doc_->GetCursor();
    if ((int)cursor.line == pos.docLine) {
        if (wordWrap_) {
            const int cursorSubRow = (cols_ > 0) ? (int)(cursor.col / (size_t)cols_) : 0;
            if (cursorSubRow == pos.subRow) {
                result.hasCursor = true;
                result.cursorCol = (int)(cursor.col - startCol);
            }
        } else {
            const int docCol = (int)cursor.col;
            if (docCol >= leftCol_ && docCol < leftCol_ + cols_) {
                result.hasCursor = true;
                result.cursorCol = docCol - leftCol_;
            }
        }
    }

    return result;
}

int DocLayout::GetLineCount() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return (int)doc_->GetLines().size();
}

int DocLayout::GetTopRow() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return topAnchor_.docLine;
}

void DocLayout::SetTopRow(int docLine)
{
    std::lock_guard<std::mutex> lk(mtx_);
    SetTopRowLocked(docLine);
}

int DocLayout::TotalVisualLinesLocked() const
{
    if (!wordWrap_) return (int)doc_->GetLines().size();
    int total = 0;
    for (const auto& line : doc_->GetLines())
        total += VisualCount(line);
    return total;
}

int DocLayout::GetTotalVisualLineCount() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return TotalVisualLinesLocked();
}

int DocLayout::GetTopVisualRow() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!wordWrap_) return topAnchor_.docLine;
    const auto& lines = doc_->GetLines();
    int visual = 0;
    for (int i = 0; i < topAnchor_.docLine && i < (int)lines.size(); ++i)
        visual += VisualCount(lines[i]);
    return visual + topAnchor_.subRow;
}

void DocLayout::SetTopVisualRow(int visualRow)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!wordWrap_) { SetTopRowLocked(visualRow); return; }
    const int total = TotalVisualLinesLocked();
    visualRow   = std::clamp(visualRow, 0, std::max(0, total - rows_));
    topAnchor_  = WalkAnchorBy({0, 0}, visualRow);
    autoScroll_ = IsAtEndLocked();
    ComputeMaxVisibleWidthLocked();
}

void DocLayout::ScrollByVisualDelta(int delta)
{
    std::lock_guard<std::mutex> lk(mtx_);
    topAnchor_  = WalkAnchorBy(topAnchor_, delta);
    autoScroll_ = IsAtEndLocked();
    ComputeMaxVisibleWidthLocked();
}

void DocLayout::ScrollToEnd()
{
    std::lock_guard<std::mutex> lk(mtx_);
    ScrollToEndLocked();
}

bool DocLayout::IsAtEnd() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return IsAtEndLocked();
}

void DocLayout::EnsureCursorVisible()
{
    std::lock_guard<std::mutex> lk(mtx_);
    EnsureCursorVisibleVertically();
    EnsureCursorVisibleHorizontally();
}

DocLayout::DocPosition
DocLayout::HitTest(int viewportRow, int viewportCol) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const ViewportAnchor pos = WalkAnchorBy(topAnchor_, viewportRow);
    const int startCol = wordWrap_ ? pos.subRow * cols_ : leftCol_;
    return {pos.docLine, startCol + viewportCol};
}

// ---------------------------------------------------------------------------
// Private *Locked helpers — called with mtx_ already held.
// ---------------------------------------------------------------------------

void DocLayout::SetTopRowLocked(int docLine)
{
    const int n = (int)doc_->GetLines().size();
    docLine     = std::clamp(docLine, 0, std::max(0, n - rows_));
    topAnchor_  = {docLine, 0};
    autoScroll_ = IsAtEndLocked();
    ComputeMaxVisibleWidthLocked();
}

void DocLayout::ScrollToEndLocked()
{
    const auto& lines = doc_->GetLines();
    if (lines.empty()) { topAnchor_ = {0, 0}; autoScroll_ = true; return; }

    const int lastDocLine = (int)lines.size() - 1;
    const int lastSubRow  = VisualCount(lines.back()) - 1;
    topAnchor_  = WalkAnchorBy({lastDocLine, lastSubRow}, -(rows_ - 1));
    autoScroll_ = true;
}

bool DocLayout::IsAtEndLocked() const
{
    const int n = (int)doc_->GetLines().size();
    if (n == 0) return true;
    return topAnchor_.docLine >= std::max(0, n - rows_);
}

void DocLayout::EnsureCursorVisibleVertically()
{
    const CursorPos cur        = doc_->GetCursor();
    const int       curDocLine = (int)cur.line;
    const int       curSubRow  = (wordWrap_ && cols_ > 0)
                               ? (int)(cur.col / (size_t)cols_) : 0;

    // Cursor above viewport — snap anchor up to cursor.
    if (curDocLine < topAnchor_.docLine ||
        (curDocLine == topAnchor_.docLine && curSubRow < topAnchor_.subRow))
    {
        topAnchor_ = {curDocLine, curSubRow};
        return;
    }

    // Walk forward rows_ visual steps; if we reach the cursor it's visible.
    ViewportAnchor pos = topAnchor_;
    for (int r = 0; r < rows_; ++r) {
        if (pos.docLine == curDocLine && pos.subRow == curSubRow) return;
        if (pos.docLine >= (int)doc_->GetLines().size()) break;
        pos = WalkAnchorBy(pos, 1);
    }

    // Cursor below viewport — place it on the last visible row.
    topAnchor_ = WalkAnchorBy({curDocLine, curSubRow}, -(rows_ - 1));
    if (topAnchor_.docLine < 0) topAnchor_ = {0, 0};
}

void DocLayout::EnsureCursorVisibleHorizontally()
{
    if (wordWrap_) return;
    const int docCol = (int)doc_->GetCursor().col;
    if (docCol < leftCol_)
        leftCol_ = docCol;
    else if (docCol >= leftCol_ + cols_)
        leftCol_ = docCol - cols_ + 1;
}

void DocLayout::ComputeMaxVisibleWidthLocked() const
{
    maxVisibleWidthDirty_ = false;
    maxVisibleWidth_ = cols_;
    if (wordWrap_) return;

    const auto& lines = doc_->GetLines();
    ViewportAnchor pos = topAnchor_;
    for (int r = 0; r < rows_ && pos.docLine < (int)lines.size(); ++r) {
        maxVisibleWidth_ = std::max(maxVisibleWidth_,
                                    (int)lines[pos.docLine].text.size());
        pos = WalkAnchorBy(pos, 1);
    }
}

int DocLayout::GetMaxVisibleWidth() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (maxVisibleWidthDirty_)
        ComputeMaxVisibleWidthLocked();
    return maxVisibleWidth_;
}

void DocLayout::SetWordWrap(bool wrap)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (wordWrap_ == wrap) return;
    wordWrap_         = wrap;
    leftCol_          = 0;
    topAnchor_.subRow = 0;  // subRow is wrap-relative; reset on mode change
    if (autoScroll_) ScrollToEndLocked();
    ComputeMaxVisibleWidthLocked();
}

bool DocLayout::GetWordWrap() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return wordWrap_;
}

void DocLayout::SetLeftCol(int col)
{
    std::lock_guard<std::mutex> lk(mtx_);
    leftCol_ = std::max(0, col);
}

int DocLayout::GetLeftCol() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return leftCol_;
}

int DocLayout::GetViewportRows() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return rows_;
}

int DocLayout::GetViewportCols() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return cols_;
}

std::vector<SearchMatch> DocLayout::Search(const std::u32string& foldedNeedle) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<SearchMatch> results;
    if (foldedNeedle.empty()) return results;

    const auto& lines = doc_->GetLines();
    for (size_t i = 0; i < lines.size(); ++i) {
        std::u32string haystack;
        haystack.reserve(lines[i].text.size());
        for (char32_t c : lines[i].text)
            haystack.push_back(CaseFold(c));

        size_t pos = 0;
        while ((pos = haystack.find(foldedNeedle, pos)) != std::u32string::npos) {
            results.push_back({i, pos, foldedNeedle.size()});
            pos += foldedNeedle.size();
        }
    }
    return results;
}

void DocLayout::SetSearchState(const std::vector<SearchMatch>& matches, size_t currentIdx)
{
    std::lock_guard<std::mutex> lk(mtx_);
    searchMatches_    = matches;
    searchCurrentIdx_ = currentIdx;
}

void DocLayout::ClearSearchState()
{
    std::lock_guard<std::mutex> lk(mtx_);
    searchMatches_.clear();
    searchCurrentIdx_ = 0;
}

int DocLayout::GetVisualRowForDocLine(int docLine) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!wordWrap_) return docLine;
    const auto& lines = doc_->GetLines();
    int visual = 0;
    for (int i = 0; i < docLine && i < (int)lines.size(); ++i)
        visual += VisualCount(lines[i]);
    return visual;
}

// ---------------------------------------------------------------------------
// Text selection
// ---------------------------------------------------------------------------

std::pair<DocLayout::DocPosition, DocLayout::DocPosition>
DocLayout::NormalizeSelectionLocked() const
{
    const DocPosition& a = selection_.anchor;
    const DocPosition& e = selection_.extent;
    if (a.docLine < e.docLine || (a.docLine == e.docLine && a.docCol <= e.docCol))
        return {a, e};
    return {e, a};
}

void DocLayout::SetSelection(const TextSelection& sel)
{
    std::lock_guard<std::mutex> lk(mtx_);
    selection_ = sel;
}

void DocLayout::ClearSelection()
{
    std::lock_guard<std::mutex> lk(mtx_);
    selection_.active = false;
}

void DocLayout::SelectAll()
{
    const int lastLine = std::max(0, GetLineCount() - 1);
    SetSelection({ DocPosition{0, 0},
                   DocPosition{lastLine, std::numeric_limits<int>::max()},
                   true });
}

DocLayout::TextSelection DocLayout::GetSelection() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return selection_;
}

bool DocLayout::HasSelection() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!selection_.active) return false;
    return selection_.anchor.docLine != selection_.extent.docLine ||
           selection_.anchor.docCol  != selection_.extent.docCol;
}

std::u32string DocLayout::GetSelectedText() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!selection_.active) return {};

    auto [selStart, selEnd] = NormalizeSelectionLocked();
    const auto& lines = doc_->GetLines();

    std::u32string result;
    for (int li = selStart.docLine; li <= selEnd.docLine && li < (int)lines.size(); ++li) {
        const std::u32string& txt = lines[li].text;
        const size_t from = (li == selStart.docLine)
                          ? std::min((size_t)selStart.docCol, txt.size()) : 0;
        const size_t to   = (li == selEnd.docLine)
                          ? std::min((size_t)selEnd.docCol, txt.size()) : txt.size();
        if (from < to)
            result += txt.substr(from, to - from);
        if (li < selEnd.docLine)
            result += U'\n';
    }
    return result;
}

void DocLayout::OnDocumentChanged(DocChangeType type, size_t lineIndex)
{
    std::lock_guard<std::mutex> lk(mtx_);
    maxVisibleWidthDirty_ = true;
    const int idx = (int)lineIndex;

    switch (type) {
    case DocChangeType::CursorMove:
    case DocChangeType::UpdateLine:
        // Always scroll to end when following output: placing the last visual
        // row at the bottom is the correct invariant for a scrollback terminal.
        // EnsureCursorVisibleVertically only moves the anchor when the cursor
        // leaves the viewport, so it leaves a phantom empty row at the bottom
        // when a wrapped line shrinks — ScrollToEndLocked never does that.
        if (autoScroll_) {
            ScrollToEndLocked();
            EnsureCursorVisibleHorizontally();
        }
        return;

    case DocChangeType::InsertLine:
        // Shift anchor when a line is inserted before it.
        if (idx <= topAnchor_.docLine)
            ++topAnchor_.docLine;
        if (autoScroll_)
            ScrollToEndLocked();
        break;

    case DocChangeType::DeleteLine: {
        const int n = (int)doc_->GetLines().size();
        if (idx < topAnchor_.docLine) {
            --topAnchor_.docLine;
        } else if (idx == topAnchor_.docLine) {
            topAnchor_.subRow  = 0;
            topAnchor_.docLine = std::min(topAnchor_.docLine, std::max(0, n - 1));
        }
        if (autoScroll_)
            ScrollToEndLocked();
        break;
    }
    }
}
