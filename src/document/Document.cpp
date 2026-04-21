#include "document/Document.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// DocLine
// ---------------------------------------------------------------------------

void DocLine::AppendInsertChar(char32_t ch)
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

MainScreenDocument::MainScreenDocument()
{
    lines_.emplace_back();
    cursor_ = {0, 0};
}

void MainScreenDocument::AppendInsertChar(char32_t ch)
{
    lines_.back().AppendInsertChar(ch);
    ++cursor_.col;
}

void MainScreenDocument::NewLine()
{
    lines_.emplace_back();
    ++cursor_.line;
    cursor_.col = 0;
}

void MainScreenDocument::SetCurrentStyle(const Style& style)
{
    lines_.back().currentStyle = style;
}

// ---------------------------------------------------------------------------
// AltScreenDocument — stub until PtyTransport + alt-screen are implemented
// ---------------------------------------------------------------------------

void AltScreenDocument::AppendInsertChar(char32_t)
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
