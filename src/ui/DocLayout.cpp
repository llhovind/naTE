#include "ui/DocLayout.h"
#include <algorithm>

DocLayout::DocLayout(Document& doc, int cols, int rows)
    : doc(doc), cols(cols), rows_(rows)
{
    doc.AddListener(this);
    Rebuild();
}

DocLayout::~DocLayout()
{
    doc.RemoveListener(this);
}

void DocLayout::SetViewportSize(int newCols, int newRows)
{
    cols  = newCols;
    rows_ = newRows;
    Rebuild();
    topRow_ = std::clamp(topRow_, 0, std::max(0, totalVisualLines_ - rows_));
    LoadWindow(topRow_);
}

// Rebuilds per-doc-line metadata (visual row counts) for all document lines.
// Cheap: one integer division per line. Does NOT expand VisualLineInfo for
// the whole document — that happens lazily in LoadWindow.
void DocLayout::Rebuild()
{
    docLineMeta_.clear();
    totalVisualLines_ = 0;
    visualLines_.clear();
    loadedWindowStart_ = 0;

    const auto& lines = doc.GetLines();
    docLineMeta_.reserve(lines.size());
    for (const auto& line : lines) {
        const size_t len  = line.text.size();
        const int    count = (len == 0) ? 1
                           : (int)((len + (size_t)cols - 1) / cols);
        docLineMeta_.push_back({count});
        totalVisualLines_ += count;
    }
}

// Loads the VisualLineInfo window centred around anchorVisualRow.
// Uses a reverse scan from the end of the document to locate the anchor doc
// line — this is O(rows_) when the anchor is near the bottom, the common
// terminal case.
void DocLayout::LoadWindow(int anchorVisualRow)
{
    visualLines_.clear();

    const auto& docLines = doc.GetLines();
    if (docLineMeta_.empty()) {
        loadedWindowStart_ = 0;
        return;
    }

    anchorVisualRow = std::clamp(anchorVisualRow, 0,
                                 std::max(0, totalVisualLines_ - 1));

    // Phase 1: reverse scan to find the doc line that contains anchorVisualRow.
    int anchorDocLine     = 0;
    int anchorDocLineVRow = 0; // visual row where anchorDocLine starts
    {
        int cumVRow = totalVisualLines_;
        for (int i = (int)docLineMeta_.size() - 1; i >= 0; --i) {
            cumVRow -= docLineMeta_[i].visualCount;
            if (cumVRow <= anchorVisualRow || i == 0) {
                anchorDocLine     = i;
                anchorDocLineVRow = cumVRow;
                break;
            }
        }
    }

    // Phase 2: walk backwards from anchorDocLine to buffer rows_ rows above.
    int startDocLine = anchorDocLine;
    int startVRow    = anchorDocLineVRow;
    while (startDocLine > 0 && startVRow > anchorVisualRow - rows_) {
        --startDocLine;
        startVRow -= docLineMeta_[startDocLine].visualCount;
    }
    startVRow = std::max(0, startVRow);
    loadedWindowStart_ = startVRow;

    // Phase 3: emit VisualLineInfo forward from startDocLine, filling 3*rows_.
    const int targetCount = rows_ * 3;
    for (int di = startDocLine;
         di < (int)docLineMeta_.size() && (int)visualLines_.size() < targetCount;
         ++di)
    {
        const auto& docLine = docLines[di];
        const size_t len    = docLine.text.size();
        if (len == 0) {
            visualLines_.push_back({di, 0});
        } else {
            for (size_t col = 0;
                 col < len && (int)visualLines_.size() < targetCount;
                 col += (size_t)cols)
            {
                visualLines_.push_back({di, col});
            }
        }
    }
}

RenderedLine DocLayout::GetRenderedLine(int visualRow)
{
    if (visualLines_.empty() ||
        visualRow < loadedWindowStart_ ||
        visualRow >= loadedWindowStart_ + (int)visualLines_.size())
    {
        LoadWindow(visualRow);
    }

    const auto&    info  = visualLines_[visualRow - loadedWindowStart_];
    const DocLine& dline = doc.GetLines()[info.docLine];
    const size_t   start = info.startCol;
    const size_t   len   = (dline.text.size() > start)
                         ? std::min(static_cast<size_t>(cols), dline.text.size() - start)
                         : 0;

    RenderedLine result;
    result.text  = dline.text.substr(start, len);
    result.attrs.assign(len, Style{});

    // Expand StyleRuns into a flat per-character array for the visible window
    // [start, start+len) only.  This is O(visible_cols + runs_in_slice),
    // regardless of how long the underlying document line is.
    for (const auto& sr : dline.styles) {
        const size_t srEnd    = sr.start + sr.length;
        const size_t sliceEnd = start + len;
        if (sr.start >= sliceEnd || srEnd <= start)
            continue;
        const size_t overlapStart = std::max(sr.start, start);
        const size_t overlapEnd   = std::min(srEnd, sliceEnd);
        for (size_t dc = overlapStart; dc < overlapEnd; ++dc)
            result.attrs[dc - start] = sr.style;
    }

    const CursorPos cursor = GetCursorPos();
    if (static_cast<int>(cursor.line) == visualRow) {
        result.hasCursor = true;
        result.cursorCol = static_cast<int>(cursor.col);
    }

    return result;
}

// Cursor position derived from docLineMeta_ without touching visualLines_.
// Reverse scan is O(1) when the cursor is on the last doc line (typical terminal).
CursorPos DocLayout::GetCursorPos() const
{
    if (docLineMeta_.empty())
        return {0, 0};

    const CursorPos docCursor   = doc.GetCursor();
    const int       cursorDocLine = static_cast<int>(docCursor.line);

    int cumVRow = totalVisualLines_;
    for (int i = (int)docLineMeta_.size() - 1; i > cursorDocLine; --i)
        cumVRow -= docLineMeta_[i].visualCount;
    cumVRow -= docLineMeta_[cursorDocLine].visualCount;
    // cumVRow = visual row where cursorDocLine starts

    const int    subRow    = (cols > 0) ? (int)(docCursor.col / (size_t)cols) : 0;
    const size_t visualCol = docCursor.col - (size_t)(subRow * cols);
    return {(size_t)(cumVRow + subRow), visualCol};
}

void DocLayout::SetTopRow(int row)
{
    topRow_     = std::clamp(row, 0, std::max(0, totalVisualLines_ - rows_));
    autoScroll_ = IsAtEnd();
    LoadWindow(topRow_);
}

void DocLayout::ScrollToEnd()
{
    topRow_     = std::max(0, totalVisualLines_ - rows_);
    autoScroll_ = true;
    LoadWindow(topRow_);
}

bool DocLayout::IsAtEnd() const
{
    return topRow_ >= std::max(0, totalVisualLines_ - rows_);
}

void DocLayout::EnsureCursorVisibleVertically()
{
    const int row = static_cast<int>(GetCursorPos().line);
    if (row < topRow_)
        SetTopRow(row);
    else if (row >= topRow_ + rows_)
        SetTopRow(row - rows_ + 1);
}

void DocLayout::EnsureCursorVisibleHorizontally()
{
    // Stub — horizontal scrolling not yet implemented.
    // Will adjust a leftCol_ offset to bring the cursor column into view.
}

void DocLayout::EnsureCursorVisible()
{
    EnsureCursorVisibleVertically();
    EnsureCursorVisibleHorizontally();
}

void DocLayout::OnDocumentChanged(DocChangeType type, size_t lineIndex)
{
    switch (type) {
    case DocChangeType::CursorMove:
        if (autoScroll_)
            EnsureCursorVisible();
        return;

    case DocChangeType::InsertLine: {
        const int idx = static_cast<int>(lineIndex);
        if (idx >= (int)docLineMeta_.size())
            docLineMeta_.push_back({1});
        else
            docLineMeta_.insert(docLineMeta_.begin() + idx, {1});
        totalVisualLines_ += 1;
        if (autoScroll_)
            ScrollToEnd();
        // When not auto-scrolling, the new line is appended after the window,
        // so the loaded window's docLine indices remain valid.
        break;
    }

    case DocChangeType::UpdateLine: {
        const int idx = static_cast<int>(lineIndex);
        if (idx < (int)docLineMeta_.size()) {
            const size_t len      = doc.GetLines()[idx].text.size();
            const int    newCount = (len == 0) ? 1
                                  : (int)((len + (size_t)cols - 1) / cols);
            const int    delta    = newCount - docLineMeta_[idx].visualCount;
            if (delta != 0) {
                docLineMeta_[idx].visualCount = newCount;
                totalVisualLines_ += delta;
                // Wrapping changed — visual positions of subsequent lines
                // shifted, so the loaded window is stale.
                visualLines_.clear();
            }
        }
        if (autoScroll_)
            EnsureCursorVisible();
        break;
    }

    case DocChangeType::DeleteLine: {
        const int idx = static_cast<int>(lineIndex);
        if (idx < (int)docLineMeta_.size()) {
            totalVisualLines_ -= docLineMeta_[idx].visualCount;
            docLineMeta_.erase(docLineMeta_.begin() + idx);
        }
        // docLine indices in the window shifted — invalidate.
        visualLines_.clear();
        topRow_ = std::clamp(topRow_, 0, std::max(0, totalVisualLines_ - rows_));
        break;
    }
    }
}
