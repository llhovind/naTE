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
    autoScroll_   = true;
    allViewDirty_ = true;
    ScrollToEndLocked();
    ComputeMaxVisibleWidthLocked();
}

// ---------------------------------------------------------------------------
// Core helpers — lock-free, called with mtx_ held.
// ---------------------------------------------------------------------------

// Visual sub-row count for one document line: 1 when wrapping is off or the
// line is empty; ceil(len / cols_) otherwise.
int DocLayout::VisualCount(const DocLine& line) const
{
    if (!wrapMode_ || cols_ <= 0) return 1;
    const int len = (int)line.text.size();
    return len == 0 ? 1 : (len + cols_ - 1) / cols_;
}

// Walk the viewport anchor forward (delta > 0) or backward (delta < 0) by
// exactly |delta| visual rows, crossing document-line boundaries as needed.
// Clamps at the document start/end — never returns a docLine outside [0, N).
DocLayout::ViewportAnchor
DocLayout::WalkAnchorBy(ViewportAnchor a, int delta) const
{
    const auto&    lines = doc_->GetLines();
    const CursorPos cur  = doc_->GetCursor();

    // Returns the visual row count for docLine, adding one extra sub-row when
    // the cursor sits exactly at a sub-row boundary end (pending-wrap state).
    // VisualCount only counts text rows; the cursor needs the empty row too.
    auto effectiveVC = [&](int docLine) {
        int vc = VisualCount(lines[docLine]);
        if (wrapMode_ && cols_ > 0 && (int)cur.line == docLine) {
            const auto& txt = lines[docLine].text;
            if (!txt.empty() && txt.size() % (size_t)cols_ == 0
                    && cur.col == txt.size())
                ++vc;
        }
        return vc;
    };

    while (delta > 0 && a.docLine < (int)lines.size()) {
        const int remaining = effectiveVC(a.docLine) - a.subRow - 1;
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
        a.subRow = effectiveVC(a.docLine) - 1;
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
    if (wrapMode_) {
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
        leftCol_     = std::max(0, cursorCol - cols_ + 1);
        leftClamped_ = (leftCol_ == 0);
    }

    if (autoScroll_) ScrollToEndLocked();
    ComputeMaxVisibleWidthLocked();
    viewDirtyRows_.assign(rows_, false);
    allViewDirty_ = true;
}

RenderedLine DocLayout::BuildRenderedLineLocked(ViewportAnchor pos) const
{
    const auto& lines = doc_->GetLines();

    if (pos.docLine >= (int)lines.size())
        return {};  // empty row — past end of document

    const DocLine& dline    = lines[pos.docLine];
    const size_t   startCol = wrapMode_
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
        hl.bgRgb = isCurrent ? searchCurrentBg_ : searchMatchBg_;
        hl.fgRgb = searchMatchFg_;
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
                selStyle.bgRgb = selectionBg_;
                selStyle.fgRgb = selectionFg_;
                for (size_t c = overlapStart; c < overlapEnd; ++c)
                    result.attrs[c - startCol] = selStyle;
            }
        }
    }

    // URL hover underline overlay — applied last so it is always visible.
    // Preserves existing fg/bg; only sets the underline flag.
    if (hoveredUrlPos_ && hoveredUrlPos_->docLine == pos.docLine && hoveredUrlLen_ > 0) {
        const size_t from = static_cast<size_t>(hoveredUrlPos_->docCol);
        const size_t to   = from + static_cast<size_t>(hoveredUrlLen_);
        const size_t sliceEnd     = startCol + len;
        const size_t overlapStart = std::max(from, startCol);
        const size_t overlapEnd   = std::min(to, sliceEnd);
        for (size_t c = overlapStart; c < overlapEnd; ++c)
            result.attrs[c - startCol].underline = true;
    }

    // Cursor: check whether the document cursor lands on this exact visual row.
    const CursorPos cursor = doc_->GetCursor();
    if ((int)cursor.line == pos.docLine) {
        if (wrapMode_) {
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

// r is viewport-relative: 0 = topmost visible line.
RenderedLine DocLayout::GetRenderedLine(int r)
{
    std::lock_guard<std::mutex> lk(mtx_);
    return BuildRenderedLineLocked(WalkAnchorBy(topAnchor_, r));
}

std::vector<RenderedLine> DocLayout::GetRenderedLines(int first, int last)
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<RenderedLine> out;
    out.reserve(static_cast<size_t>(last - first));
    ViewportAnchor pos = WalkAnchorBy(topAnchor_, first);
    for (int r = first; r < last; ++r) {
        out.push_back(BuildRenderedLineLocked(pos));
        pos = WalkAnchorBy(pos, 1);
    }
    return out;
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
    if (!wrapMode_) return (int)doc_->GetLines().size();
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
    if (!wrapMode_) return topAnchor_.docLine;
    const auto& lines = doc_->GetLines();
    int visual = 0;
    for (int i = 0; i < topAnchor_.docLine && i < (int)lines.size(); ++i)
        visual += VisualCount(lines[i]);
    return visual + topAnchor_.subRow;
}

void DocLayout::SetTopVisualRow(int visualRow)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!wrapMode_) { SetTopRowLocked(visualRow); return; }
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
    leftClamped_ = true;   // user interaction: resume horizontal cursor tracking
    EnsureCursorVisibleVertically();
    EnsureCursorVisibleHorizontally();
}

DocLayout::DocPosition
DocLayout::HitTest(int viewportRow, int viewportCol) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const ViewportAnchor pos = WalkAnchorBy(topAnchor_, viewportRow);
    const int startCol = wrapMode_ ? pos.subRow * cols_ : leftCol_;
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
    int       lastSubRow  = VisualCount(lines.back()) - 1;
    // If the cursor is at a sub-row boundary end of the last DocLine, add the
    // extra empty sub-row so it is included in the viewport.
    if (wrapMode_ && cols_ > 0) {
        const CursorPos cur = doc_->GetCursor();
        const auto&     txt = lines.back().text;
        if ((int)cur.line == lastDocLine && !txt.empty()
                && txt.size() % (size_t)cols_ == 0 && cur.col == txt.size())
            ++lastSubRow;
    }
    topAnchor_  = WalkAnchorBy({lastDocLine, lastSubRow}, -(rows_ - 1));
    // Never auto-scroll above the current canvas origin — old scrollback stays hidden.
    const int origin = static_cast<int>(doc_->GetScrollbackOrigin());
    if (topAnchor_.docLine < origin)
        topAnchor_ = { origin, 0 };
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
    const int       curSubRow  = (wrapMode_ && cols_ > 0)
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
    if (wrapMode_) return;
    if (!leftClamped_) return;  // viewport pinned by user; skip cursor tracking in both directions
    static constexpr int kMargin = 5;
    const int docCol = (int)doc_->GetCursor().col;
    if (docCol < leftCol_ + kMargin) {
        leftCol_ = std::max(0, docCol - kMargin);
    } else if (docCol >= leftCol_ + cols_ - kMargin) {
        leftCol_ = std::max(0, docCol - cols_ + kMargin + 1);
    }
}

void DocLayout::ComputeMaxVisibleWidthLocked() const
{
    maxVisibleWidthDirty_ = false;
    maxVisibleWidth_ = cols_;
    if (wrapMode_) return;

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

void DocLayout::SetWrapMode(bool wrap)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (wrapMode_ == wrap) return;
    wrapMode_         = wrap;
    leftCol_          = 0;
    leftClamped_      = true;
    topAnchor_.subRow = 0;  // subRow is wrap-relative; reset on mode change
    if (autoScroll_) ScrollToEndLocked();
    ComputeMaxVisibleWidthLocked();
}

bool DocLayout::GetWrapMode() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return wrapMode_;
}

void DocLayout::SetLeftCol(int col)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (maxVisibleWidthDirty_)
        ComputeMaxVisibleWidthLocked();
    const int maxLeft = std::max(0, maxVisibleWidth_ - cols_);
    leftCol_     = std::clamp(col, 0, maxLeft);
    leftClamped_ = (leftCol_ == 0);
}

void DocLayout::SetLeftColRaw(int col)
{
    std::lock_guard<std::mutex> lk(mtx_);
    leftCol_ = std::clamp(col, 0, std::max(0, maxVisibleWidth_ - cols_));
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

// ---------------------------------------------------------------------------
// URL hover
// ---------------------------------------------------------------------------

std::optional<UrlScanner::UrlSpan> DocLayout::FindUrlAt(DocPosition pos) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto& lines = doc_->GetLines();
    if (pos.docLine < 0 || pos.docLine >= (int)lines.size()) return std::nullopt;
    const auto spans = UrlScanner::ScanLine(lines[pos.docLine].text);
    for (const auto& span : spans) {
        if ((size_t)pos.docCol >= span.col && (size_t)pos.docCol < span.col + span.len)
            return span;
    }
    return std::nullopt;
}

void DocLayout::SetHoveredUrl(std::optional<DocPosition> urlStart, int len)
{
    std::lock_guard<std::mutex> lk(mtx_);

    const bool samePos = hoveredUrlPos_.has_value() == urlStart.has_value()
                      && (!urlStart || (urlStart->docLine == hoveredUrlPos_->docLine
                                     && urlStart->docCol  == hoveredUrlPos_->docCol))
                      && hoveredUrlLen_ == len;
    if (samePos) return;

    if (hoveredUrlPos_)
        MarkDocLineDirtyLocked(hoveredUrlPos_->docLine);

    hoveredUrlPos_ = urlStart;
    hoveredUrlLen_ = len;

    if (hoveredUrlPos_)
        MarkDocLineDirtyLocked(hoveredUrlPos_->docLine);
}

int DocLayout::GetVisualRowForDocLine(int docLine) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!wrapMode_) return docLine;
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

void DocLayout::SetHighlightColors(const UiColors& u)
{
    std::lock_guard<std::mutex> lk(mtx_);
    selectionBg_     = u.selectionBg;
    selectionFg_     = u.selectionFg;
    searchMatchBg_   = u.searchMatchBg;
    searchMatchFg_   = u.searchMatchFg;
    searchCurrentBg_ = u.searchCurrentBg;
    allViewDirty_    = true;
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

// Mark the viewport rows that correspond to docLine as dirty.
// No-op if docLine is not currently visible.  Assumes mtx_ is held.
void DocLayout::MarkDocLineDirtyLocked(int docLine)
{
    if ((int)viewDirtyRows_.size() != rows_)
        viewDirtyRows_.assign(rows_, false);

    // Non-wrap: each docLine maps to exactly one viewport row (O(1) lookup).
    if (!wrapMode_) {
        const int viewRow = docLine - topAnchor_.docLine;
        if (viewRow >= 0 && viewRow < rows_)
            viewDirtyRows_[viewRow] = true;
        return;
    }

    // Wrap mode: walk the viewport once, marking every visual row that
    // belongs to the target docLine.  Stop as soon as we pass it.
    ViewportAnchor pos = topAnchor_;
    const auto& lines = doc_->GetLines();
    for (int r = 0; r < rows_; ++r) {
        if (pos.docLine >= (int)lines.size()) break;
        if (pos.docLine > docLine) break;
        if (pos.docLine == docLine)
            viewDirtyRows_[r] = true;
        pos = WalkAnchorBy(pos, 1);
    }
}

// Called on the UI thread once per frame.  Returns the accumulated dirty state
// since the last call and atomically resets it to clean.
DocLayout::DirtySnapshot DocLayout::TakeAndClearDirty()
{
    std::lock_guard<std::mutex> lk(mtx_);
    DirtySnapshot snap;
    snap.all  = allViewDirty_;
    snap.rows = viewDirtyRows_;
    allViewDirty_ = false;
    viewDirtyRows_.assign(rows_, false);
    return snap;
}

void DocLayout::OnDocumentChanged(DocChangeType type, size_t lineIndex)
{
    std::lock_guard<std::mutex> lk(mtx_);
    maxVisibleWidthDirty_ = true;
    const int idx = (int)lineIndex;

    switch (type) {
    case DocChangeType::CursorMove:
        // Cursor-only change: no content modified.  If the anchor doesn't move
        // we only need to repaint the cursor row, but since we don't carry the
        // old cursor position we conservatively mark the whole viewport dirty.
        if (autoScroll_) {
            const ViewportAnchor before = topAnchor_;
            ScrollToEndLocked();
            EnsureCursorVisibleHorizontally();
            if (!(topAnchor_ == before)) { allViewDirty_ = true; return; }
        }
        allViewDirty_ = true;
        return;

    case DocChangeType::UpdateLine:
        // Always scroll to end when following output: placing the last visual
        // row at the bottom is the correct invariant for a scrollback terminal.
        // EnsureCursorVisibleVertically only moves the anchor when the cursor
        // leaves the viewport, so it leaves a phantom empty row at the bottom
        // when a wrapped line shrinks — ScrollToEndLocked never does that.
        if (autoScroll_) {
            const ViewportAnchor before = topAnchor_;
            ScrollToEndLocked();
            EnsureCursorVisibleHorizontally();
            if (!(topAnchor_ == before)) { allViewDirty_ = true; return; }
        }
        // Anchor unchanged — only the row(s) for this docLine changed.
        MarkDocLineDirtyLocked(idx);
        return;

    case DocChangeType::InsertLine:
        // Shift anchor when a line is inserted before it.
        if (idx <= topAnchor_.docLine)
            ++topAnchor_.docLine;
        if (autoScroll_)
            ScrollToEndLocked();
        allViewDirty_ = true;
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
        allViewDirty_ = true;
        break;
    }

    case DocChangeType::CanvasReset:
        // A new canvas started at idx.  The document is authoritative for the
        // origin (GetScrollbackOrigin() == idx at this point); set the anchor
        // directly so SetTopRowLocked's clamp logic doesn't interfere.
        topAnchor_    = { static_cast<int>(idx), 0 };
        autoScroll_   = true;
        allViewDirty_ = true;
        return;

    default: break;
    }
}
