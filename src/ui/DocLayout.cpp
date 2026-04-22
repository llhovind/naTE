#include "ui/DocLayout.h"
#include <algorithm>

DocLayout::DocLayout(Document& doc, int cols)
    : doc(doc), cols(cols)
{
    doc.AddListener(this);
}

DocLayout::~DocLayout()
{
    doc.RemoveListener(this);
}

void DocLayout::Rebuild(int cols)
{
    this->cols = cols;
    visualLines.clear();

    const auto& lines = doc.GetLines();
    for (int i = 0; i < (int)lines.size(); ++i)
        RebuildLine(i);
}

void DocLayout::RebuildLine(int lineIndex)
{
    auto first = std::lower_bound(visualLines.begin(), visualLines.end(), lineIndex,
        [](const VisualLineInfo& v, int idx) { return v.docLine < idx; });
    auto last = first;
    while (last != visualLines.end() && last->docLine == lineIndex) ++last;

    const auto& line = doc.GetLines()[lineIndex];
    const size_t len = line.text.size();
    std::vector<VisualLineInfo> fresh;
    if (len == 0) {
        fresh.push_back({lineIndex, 0});
    } else {
        for (size_t start = 0; start < len; start += cols)
            fresh.push_back({lineIndex, start});
    }

    auto pos = visualLines.erase(first, last);
    visualLines.insert(pos, fresh.begin(), fresh.end());
}

void DocLayout::OnDocumentChanged(DocChangeType type, size_t lineIndex)
{
    switch (type) {
    case DocChangeType::CursorMove:
        return;

    case DocChangeType::InsertLine:
        visualLines.push_back({static_cast<int>(lineIndex), 0});
        break;

    case DocChangeType::UpdateLine:
        RebuildLine(static_cast<int>(lineIndex));
        break;

    case DocChangeType::DeleteLine: {
        const int idx = static_cast<int>(lineIndex);
        auto first = std::lower_bound(visualLines.begin(), visualLines.end(), idx,
            [](const VisualLineInfo& v, int i) { return v.docLine < i; });
        auto last = first;
        while (last != visualLines.end() && last->docLine == idx) ++last;
        auto after = visualLines.erase(first, last);
        for (auto it = after; it != visualLines.end(); ++it) --it->docLine;
        break;
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
