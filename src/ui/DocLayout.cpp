#include "ui/DocLayout.h"
#include <algorithm>

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

void DocLayout::ComputeMaxVisibleWidthLocked()
{
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

void DocLayout::OnDocumentChanged(DocChangeType type, size_t lineIndex)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const int idx = (int)lineIndex;

    switch (type) {
    case DocChangeType::CursorMove:
        if (autoScroll_) {
            EnsureCursorVisibleVertically();
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

    case DocChangeType::UpdateLine:
        // In word-wrap mode the anchor line may now have fewer sub-rows.
        if (wordWrap_ && idx == topAnchor_.docLine) {
            const auto& lines  = doc_->GetLines();
            const int   maxSub = VisualCount(lines[idx]) - 1;
            topAnchor_.subRow  = std::min(topAnchor_.subRow, maxSub);
        }
        if (autoScroll_) {
            EnsureCursorVisibleVertically();
            EnsureCursorVisibleHorizontally();
        }
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
