#include "document/Document.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// DocLine
// ---------------------------------------------------------------------------

void DocLine::AppendChar(char32_t ch)
{
    size_t pos = text.size();
    text.push_back(ch);

    if (!styles.empty()) {
        StyleRun& last = styles.back();
        if (last.style == currentStyle && last.start + last.length == pos) {
            last.length += 1;
            return;
        }
    }

    styles.push_back({pos, 1, currentStyle});
}

// ---------------------------------------------------------------------------
// MainScreenDocument
// ---------------------------------------------------------------------------

void MainScreenDocument::AppendChar(char32_t ch)
{
    if (lines_.empty())
        lines_.emplace_back();
    lines_.back().AppendChar(ch);
}

void MainScreenDocument::NewLine()
{
    lines_.emplace_back();
}

void MainScreenDocument::SetCurrentStyle(const Style& style)
{
    if (lines_.empty())
        lines_.emplace_back();
    lines_.back().currentStyle = style;
}

// ---------------------------------------------------------------------------
// AltScreenDocument — stub until PtyTransport + alt-screen are implemented
// ---------------------------------------------------------------------------

void AltScreenDocument::AppendChar(char32_t)
{
    throw std::logic_error("AltScreenDocument not yet implemented");
}

void AltScreenDocument::NewLine()
{
    throw std::logic_error("AltScreenDocument not yet implemented");
}

void AltScreenDocument::SetCurrentStyle(const Style&)
{
    throw std::logic_error("AltScreenDocument not yet implemented");
}

const std::deque<DocLine>& AltScreenDocument::GetLines() const
{
    throw std::logic_error("AltScreenDocument not yet implemented");
}
