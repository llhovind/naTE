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
    DocLayout(Document& doc, int cols);
    ~DocLayout() override;

    void Rebuild(int cols);
    DocLayoutLine GetLine(int visualRow) const;
    CursorPos GetCursorPos() const;

    int GetLineCount() const { return (int)visualLines.size(); }

    void OnDocumentChanged(DocChangeType type, size_t lineIndex) override;

private:
    void RebuildLine(int lineIndex);

    Document& doc;
    int cols;

    std::vector<VisualLineInfo> visualLines;
};
