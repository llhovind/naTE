#pragma once

#include "document/Document.h"
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

class DocLayout {
public:
    DocLayout(const Document& doc, int cols);

    void Rebuild(int cols);
    DocLayoutLine GetLine(int visualRow) const;
    CursorPos GetCursorPos() const;

    int GetLineCount() const { return (int)visualLines.size(); }

private:
    const Document& doc;
    int cols;

    std::vector<VisualLineInfo> visualLines;
};
