#include "ui/Layout.h"
#include <iostream>
#include <algorithm>

Layout::Layout(const Document& doc, int cols)
    : doc(doc), cols(cols)
{
}

void Layout::Rebuild(int cols)
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

LayoutLine Layout::GetLine(int visualRow) const
{
    const auto& info = visualLines[visualRow];
    return LayoutLine(doc.GetLines()[info.docLine], info.startCol, cols);
}

// --------------------------------------------

LayoutLine::LayoutLine(const DocLine& line, size_t startCol, int cols)
    : line(line), startCol(startCol), cols(cols)
{
}

void LayoutLine::Render() const
{
    size_t docStart = startCol;
    size_t docEnd = std::min(docStart + cols, line.text.size());

    size_t runIndex = 0;
    const StyleRun* run = nullptr;

    // find first relevant run
    while (runIndex < line.styles.size()) {
        const auto& r = line.styles[runIndex];
        if (r.start + r.length > docStart) {
            run = &r;
            break;
        }
        runIndex++;
    }

    for (size_t docCol = docStart; docCol < docEnd; ++docCol)
    {
        while (run && docCol >= run->start + run->length) {
            runIndex++;
            run = (runIndex < line.styles.size())
                ? &line.styles[runIndex]
                : nullptr;
        }

        char ch = (char)line.text[docCol];

        // crude style demo
        if (run && run->style.bold)
            std::cout << (char)toupper(ch);
        else
            std::cout << ch;
    }

    std::cout << "\n";
}