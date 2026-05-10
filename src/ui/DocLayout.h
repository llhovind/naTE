#pragma once

#include "document/Document.h"
#include "document/IDocumentListener.h"
#include "ui/SearchMatch.h"
#include <mutex>
#include <vector>

// Rendered representation of one visual row — the only line-level type that
// crosses the DocLayout→TerminalPanel boundary.  attrs is parallel to text:
// attrs[i] is the Style for text[i].  Both are bounded by the viewport width,
// never by the underlying document line length.
struct RenderedLine {
    std::u32string     text;
    std::vector<Style> attrs;
    bool               hasCursor = false;
    int                cursorCol = 0;
};

class DocLayout : public IDocumentListener {
public:
    DocLayout(Document& doc, int cols = 80, int rows = 24);
    ~DocLayout() override;

    // Call when the visible terminal size changes.
    void SetViewportSize(int cols, int rows);

    // Returns the rendered content for one visual row.
    // r is viewport-relative: 0 = topmost visible line.
    // Returns an empty RenderedLine (text empty, hasCursor false) when r
    // points past the end of the document.
    RenderedLine GetRenderedLine(int r);

    void SetDocument(Document& newDoc);

    // Document line count.
    int GetLineCount() const;

    CursorPos GetCursorDocPos() const { return doc_->GetCursor(); }

    // Viewport state in document-line coordinates (wrap mode unaware).
    int  GetTopRow() const;
    void SetTopRow(int docLine);

    // Viewport state in visual-row coordinates. When wrap mode is off these
    // are identical to the doc-line forms (every logical line is one visual row).
    int  GetTotalVisualLineCount() const;
    int  GetTopVisualRow()         const;
    void SetTopVisualRow(int visualRow);
    void ScrollByVisualDelta(int delta);
    void ScrollToEnd();
    bool IsAtEnd() const;
    void EnsureCursorVisible();

    void SetwrapMode(bool wrap);
    bool GetwrapMode() const;
    void SetLeftCol(int col);
    int  GetLeftCol() const;
    int  GetMaxVisibleWidth() const;
    int  GetViewportRows() const;
    int  GetViewportCols() const;

    // Search — caller passes a case-folded needle; DocLayout folds each haystack
    // character internally so both sides are folded before comparison.
    std::vector<SearchMatch> Search(const std::u32string& foldedNeedle) const;
    void SetSearchState(const std::vector<SearchMatch>& matches, size_t currentIdx);
    void ClearSearchState();

    // Returns the first visual row that corresponds to docLine (sub-row 0).
    int GetVisualRowForDocLine(int docLine) const;

    // Translate a viewport-relative (col, row) to a document position.
    struct DocPosition { int docLine = 0; int docCol = 0; };
    DocPosition HitTest(int viewportRow, int viewportCol) const;

    // Text selection — anchor is where the drag began, extent is the current end.
    // Either may precede the other; use GetSelectedText() for the normalised extract.
    struct TextSelection {
        DocPosition anchor;
        DocPosition extent;
        bool        active = false;
    };

    void           SetSelection(const TextSelection& sel);
    void           ClearSelection();
    void           SelectAll();
    TextSelection  GetSelection() const;
    std::u32string GetSelectedText() const;
    bool           HasSelection() const;

    void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;

private:
    // Position in document space: which doc line, and which visual sub-row
    // within that line (always 0 when wrap mode is off).
    struct ViewportAnchor { int docLine = 0; int subRow = 0; };

    // All *Locked methods assume mtx_ is already held by the caller.
    ViewportAnchor WalkAnchorBy(ViewportAnchor a, int delta) const;
    int            VisualCount(const DocLine& line) const;
    int            TotalVisualLinesLocked() const;
    void           ComputeMaxVisibleWidthLocked() const;
    void           SetTopRowLocked(int docLine);
    void           ScrollToEndLocked();
    bool           IsAtEndLocked() const;
    void           EnsureCursorVisibleVertically();
    void           EnsureCursorVisibleHorizontally();

    mutable std::mutex mtx_;

    Document*      doc_;
    int            cols_;
    int            rows_;
    ViewportAnchor topAnchor_;
    int            leftCol_              = 0;
    mutable int    maxVisibleWidth_      = 0;
    mutable bool   maxVisibleWidthDirty_ = true;
    bool           autoScroll_           = true;
    bool           wrapMode_             = false;

    std::vector<SearchMatch> searchMatches_;
    size_t                   searchCurrentIdx_ = 0;

    TextSelection selection_;

    // Returns {start, end} with start ≤ end in document order. Caller holds mtx_.
    std::pair<DocPosition, DocPosition> NormalizeSelectionLocked() const;
};
