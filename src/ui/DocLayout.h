#pragma once

#include "document/Document.h"
#include "document/IDocumentListener.h"
#include <vector>

struct VisualLineInfo {
    int docLine;
    size_t startCol;
};

class DocLayoutLine {
public:
    DocLayoutLine(const DocLine& line, size_t startCol, int cols);

    const DocLine& line;
    size_t startCol;
    int cols;
};

class DocLayout : public IDocumentListener {
public:
    DocLayout(Document& doc, int cols = 80, int rows = 24);
    ~DocLayout() override;

    // Call when the visible terminal size changes. Rebuilds layout metadata
    // and re-clamps the viewport.
    void SetViewportSize(int cols, int rows);

    // Returns the VisualLineInfo for a given visual row, loading the viewport
    // window lazily if the row is outside the current loaded range.
    DocLayoutLine GetLine(int visualRow);

    CursorPos GetCursorPos() const;
    int GetLineCount() const { return totalVisualLines_; }

    // Viewport state — DocLayout is the single source of truth.
    int  GetTopRow() const { return topRow_; }
    void SetTopRow(int row);            // clamps; updates autoScroll_
    void ScrollToEnd();
    bool IsAtEnd() const;
    void EnsureCursorVisible();

    void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;

private:
    void Rebuild();
    void LoadWindow(int anchorVisualRow);
    void EnsureCursorVisibleVertically();
    void EnsureCursorVisibleHorizontally(); // stub for future horizontal scroll

    Document& doc;
    int cols;
    int rows_;
    int topRow_      = 0;
    bool autoScroll_ = true;

    // Per-document-line metadata: how many visual rows each doc line occupies.
    // Always fully maintained; never trimmed to the viewport.
    struct DocLineMeta {
        int visualCount;
    };
    std::vector<DocLineMeta> docLineMeta_;
    int totalVisualLines_ = 0;

    // Lazy viewport window: VisualLineInfo only for the rows near topRow_.
    // Reloaded on scroll or when invalidated by a document change.
    std::vector<VisualLineInfo> visualLines_;
    int loadedWindowStart_ = 0;
};
