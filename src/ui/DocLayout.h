#pragma once

#include "document/Document.h"
#include "document/IDocumentListener.h"
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

    // Call when the visible terminal size changes. Rebuilds layout metadata
    // and re-clamps the viewport.
    void SetViewportSize(int cols, int rows);

    // Returns the rendered content for one visual row.  Loads the viewport
    // window lazily if the row is outside the current loaded range.
    RenderedLine GetRenderedLine(int visualRow);

    int  GetLineCount() const;
    CursorPos GetCursorDocPos() const { return doc.GetCursor(); }

    // Viewport state — DocLayout is the single source of truth.
    int  GetTopRow() const;
    void SetTopRow(int row);
    void ScrollToEnd();
    bool IsAtEnd() const;
    void EnsureCursorVisible();

    void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;

private:
    struct VisualLineInfo {
        int    docLine;
        size_t startCol;
    };

    // All *Locked methods assume mtx_ is already held by the caller.
    void      Rebuild();
    void      LoadWindow(int anchorVisualRow);
    void      SetTopRowLocked(int row);
    void      ScrollToEndLocked();
    bool      IsAtEndLocked() const;
    void      EnsureCursorVisibleVertically();
    void      EnsureCursorVisibleHorizontally();
    CursorPos GetCursorPos() const;

    mutable std::mutex mtx_;

    Document& doc;
    int cols;
    int rows_;
    int  topRow_     = 0;
    bool autoScroll_ = true;

    // Per-document-line metadata: how many visual rows each doc line occupies.
    // Always fully maintained; never trimmed to the viewport.
    struct DocLineMeta { int visualCount; };
    std::vector<DocLineMeta> docLineMeta_;
    int totalVisualLines_ = 0;

    // Lazy viewport window: VisualLineInfo only for the rows near topRow_.
    // Reloaded on scroll or when invalidated by a document change.
    std::vector<VisualLineInfo> visualLines_;
    int loadedWindowStart_ = 0;
};
