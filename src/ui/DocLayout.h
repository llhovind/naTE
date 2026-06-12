#pragma once

#include "config/Color.h"
#include "document/Document.h"
#include "document/IDocumentListener.h"
#include "ui/SearchMatch.h"
#include "ui/UrlScanner.h"
#include <mutex>
#include <shared_mutex>
#include <optional>
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

    // Returns rendered content for [first, last) visual rows in one lock
    // acquisition, walking the anchor incrementally (O(rows) instead of
    // O(rows²)).  Prefer this over repeated GetRenderedLine calls in paint loops.
    std::vector<RenderedLine> GetRenderedLines(int first, int last);

    void SetDocument(Document& newDoc);

    // Document line count.
    int GetLineCount() const;

    // Locks mtx_ for the doc_ pointer (swapped on alt-screen switch), then
    // takes a coherent cursor snapshot via Document::GetCursor().
    CursorPos GetCursorDocPos() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return doc_->GetCursor();
    }

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

    void SetWrapMode(bool wrap);
    bool GetWrapMode() const;
    void SetLeftCol(int col);
    void SetLeftColRaw(int col);  // sets leftCol_ without modifying leftClamped_; for animation only
    int  GetLeftCol() const;
    int  GetMaxVisibleWidth() const;
    int  GetViewportRows() const;
    int  GetViewportCols() const;

    // Search — caller passes a case-folded needle; DocLayout folds each haystack
    // character internally so both sides are folded before comparison.
    std::vector<SearchMatch> Search(const std::u32string& foldedNeedle) const;
    // Scan only lines [fromLine, toLine). Same folding contract as Search().
    std::vector<SearchMatch> SearchRange(const std::u32string& foldedNeedle,
                                         size_t fromLine, size_t toLine) const;
    void SetSearchState(const std::vector<SearchMatch>& matches, size_t currentIdx);
    void ClearSearchState();

    // Returns the first visual row that corresponds to docLine (sub-row 0).
    int GetVisualRowForDocLine(int docLine) const;

    // Translate a viewport-relative (col, row) to a document position.
    struct DocPosition { int docLine = 0; int docCol = 0; };
    DocPosition HitTest(int viewportRow, int viewportCol) const;

    // Returns the UrlScanner::UrlSpan at pos, if any (scans the doc line on demand).
    std::optional<UrlScanner::UrlSpan> FindUrlAt(DocPosition pos) const;

    // Set/clear the hovered URL. Marks affected viewport rows dirty for repaint.
    void SetHoveredUrl(std::optional<DocPosition> urlStart, int len = 0);

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

    // Double-click: find the regex match in pos.docLine that contains pos.docCol
    // and set it as the active selection. Returns true if a non-empty word was found.
    bool SelectWordAt(DocPosition pos, const std::string& regexPattern);

    // Triple-click: select the entire logical line at pos (col 0 → text.size()).
    void SelectLineAt(DocPosition pos);

    // Update selection and search highlight colors from the active theme.
    // Called by TerminalPanel::ApplyConfig.
    void SetHighlightColors(const UiColors& u);

    void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;

    // Snapshot of which viewport rows need repainting.  Consumed once per frame
    // by TerminalPanel::OnDocumentUpdate; atomically cleared on read.
    struct DirtySnapshot {
        bool              all  = true;
        std::vector<bool> rows;  // per-viewport-row; meaningful only when !all
    };
    DirtySnapshot TakeAndClearDirty();

private:
    // Position in document space: which doc line, and which visual sub-row
    // within that line (always 0 when wrap mode is off).
    struct ViewportAnchor {
        int docLine = 0;
        int subRow  = 0;
        bool operator==(const ViewportAnchor& o) const {
            return docLine == o.docLine && subRow == o.subRow;
        }
    };

    // All *Locked methods assume mtx_ is already held by the caller.
    std::vector<SearchMatch> SearchRangeLocked(const std::u32string& foldedNeedle,
                                               size_t fromLine, size_t toLine) const;
    ViewportAnchor WalkAnchorBy(ViewportAnchor a, int delta) const;
    RenderedLine   BuildRenderedLineLocked(ViewportAnchor pos) const;
    int            VisualCount(const DocLine& line) const;
    int            TotalVisualLinesLocked() const;
    void           ComputeMaxVisibleWidthLocked() const;
    void           SetTopRowLocked(int docLine);
    void           ScrollToEndLocked();
    bool           IsAtEndLocked() const;
    void           EnsureCursorVisibleVertically();
    void           EnsureCursorVisibleHorizontally();
    void           MarkDocLineDirtyLocked(int docLine);

    mutable std::mutex mtx_;

    Document*      doc_;
    int            cols_;
    int            rows_;
    ViewportAnchor topAnchor_;
    int            leftCol_              = 0;
    mutable int    maxVisibleWidth_      = 0;
    mutable bool   maxVisibleWidthDirty_ = true;
    bool           autoScroll_           = true;
    bool           leftClamped_          = true;   // set only by SetLeftCol(); suppresses left-scroll when false
    bool           wrapMode_             = false;

    bool              allViewDirty_  = true;
    std::vector<bool> viewDirtyRows_;         // size == rows_, protected by mtx_

    std::vector<SearchMatch> searchMatches_;
    size_t                   searchCurrentIdx_ = 0;

    // Theme-derived highlight colors — updated via SetHighlightColors().
    Rgb selectionBg_     = UiColors{}.selectionBg;
    Rgb selectionFg_     = UiColors{}.selectionFg;
    Rgb searchMatchBg_   = UiColors{}.searchMatchBg;
    Rgb searchMatchFg_   = UiColors{}.searchMatchFg;
    Rgb searchCurrentBg_ = UiColors{}.searchCurrentBg;

    TextSelection              selection_;
    std::optional<DocPosition> hoveredUrlPos_;
    int                        hoveredUrlLen_ = 0;

    // Returns {start, end} with start ≤ end in document order. Caller holds mtx_.
    std::pair<DocPosition, DocPosition> NormalizeSelectionLocked() const;
};
