#pragma once

#include "document/Document.h"
#include <vector>

struct VisualLineInfo {
    int docLine;
    size_t startCol;
};

// class LayoutLine;
class LayoutLine {
public:
    LayoutLine(const DocLine& line, size_t startCol, int cols);

    const DocLine& line;
    size_t startCol;
    int cols;


// private:
//     const DocLine& line;
//     size_t startCol;
//     int cols;
};

class Layout {
public:
    Layout(const Document& doc, int cols);

    void Rebuild(int cols);
    LayoutLine GetLine(int visualRow) const;

    int GetLineCount() const { return (int)visualLines.size(); }

private:
    const Document& doc;
    int cols;

    std::vector<VisualLineInfo> visualLines;
};

