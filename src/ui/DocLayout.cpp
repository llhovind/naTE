#include "ui/DocLayout.h"
#include <algorithm>

DocLayout::DocLayout(const Document& doc, int cols)
    : doc(doc), cols(cols)
{
}

void DocLayout::Rebuild(int cols)
{
    this->cols = cols;
    visualLines.clear();

    const auto& lines = doc.GetLines();

    for (int i = 0; i < (int)lines.size(); ++i)
    {
        const auto& line = lines[i];
        size_t len = line.text.size();

        if (len == 0) {
            visualLines.push_back({i, 0});
            continue;
        }

        for (size_t start = 0; start < len; start += cols)
        {
            visualLines.push_back({i, start});
        }
    }
}

DocLayoutLine DocLayout::GetLine(int visualRow) const
{
    const auto& info = visualLines[visualRow];
    return DocLayoutLine(doc.GetLines()[info.docLine], info.startCol, cols);
}

DocLayoutLine::DocLayoutLine(const DocLine& line, size_t startCol, int cols)
    : line(line), startCol(startCol), cols(cols)
{
}

CursorPos DocLayout::GetCursorPos() const
{
    if (visualLines.empty())
        return {0, 0};

    const CursorPos docCursor = doc.GetCursor();

    int visualRow = 0;
    for (int i = (int)visualLines.size() - 1; i >= 0; --i) {
        const auto& info = visualLines[i];
        if ((size_t)info.docLine == docCursor.line && info.startCol <= docCursor.col) {
            visualRow = i;
            break;
        }
    }

    const size_t visualCol = docCursor.col - visualLines[visualRow].startCol;
    return {(size_t)visualRow, visualCol};
}
