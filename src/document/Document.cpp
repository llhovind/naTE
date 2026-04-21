#include "document/Document.h"

void DocLine::AppendChar(char32_t ch)
{
    size_t pos = text.size();
    text.push_back(ch);

    if (!styles.empty()) {
        StyleRun& last = styles.back();

        if (last.style == currentStyle &&
            last.start + last.length == pos)
        {
            last.length += 1;
            return;
        }
    }

    styles.push_back({pos, 1, currentStyle});
}

void Document::AppendChar(char32_t ch)
{
    if (lines.empty())
        lines.emplace_back();

    lines.back().AppendChar(ch);
}

void Document::NewLine()
{
    lines.emplace_back();
}

void Document::SetCurrentStyle(const Style& style)
{
    if (lines.empty())
        lines.emplace_back();

    lines.back().currentStyle = style;
}