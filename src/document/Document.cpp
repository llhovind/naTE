#include "document/Document.h"
#include <algorithm>
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

void DocLine::DeletePreviousChar(size_t cursorCol)
{
    if (cursorCol == 0 || text.empty())
        return;

    const size_t erasePos = cursorCol - 1;
    if (erasePos >= text.size())
        return;

    text.erase(erasePos, 1);

    for (auto it = styles.begin(); it != styles.end(); ) {
        const size_t runEnd = it->start + it->length;
        if (it->start > erasePos) {
            --it->start;
            ++it;
        } else if (runEnd > erasePos) {
            --it->length;
            it = (it->length == 0) ? styles.erase(it) : ++it;
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Document — listener management
// ---------------------------------------------------------------------------

void Document::SetTitle(const std::string& title)
{
    title_ = title;
    NotifyListeners(DocChangeType::TitleChanged, 0);
}

void Document::AddListener(IDocumentListener* l)
{
    listeners_.push_back(l);
}

void Document::RemoveListener(IDocumentListener* l)
{
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
}

void Document::NotifyListeners(DocChangeType type, size_t lineIndex)
{
    for (auto* l : listeners_)
        l->OnDocumentChanged(type, lineIndex);
}

// ---------------------------------------------------------------------------
// MainScreenDocument
// ---------------------------------------------------------------------------

MainScreenDocument::MainScreenDocument(int maxLines)
    : maxLines_(maxLines)
{
    lines_.emplace_back();
    cursor_ = {0, 0};
}

void MainScreenDocument::AppendInsertChar(char32_t ch)
{
    lines_.back().AppendInsertChar(ch);
    ++cursor_.col;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::Backspace()
{
    DocLine& line = lines_.back();
    if (cursor_.col == 0 || line.text.empty())
        return;

    line.DeletePreviousChar(cursor_.col);
    --cursor_.col;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::NewLine()
{
    lines_.emplace_back();
    ++cursor_.line;
    cursor_.col = 0;
    NotifyListeners(DocChangeType::InsertLine, cursor_.line);

    if ((int)lines_.size() > maxLines_) {
        lines_.pop_front();
        --cursor_.line;
        NotifyListeners(DocChangeType::DeleteLine, 0);
    }
}

void MainScreenDocument::SetCurrentStyle(const Style& style)
{
    lines_.back().currentStyle = style;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

// ---------------------------------------------------------------------------
// AltScreenDocument — stub until PtyTransport + alt-screen are implemented
// ---------------------------------------------------------------------------

void AltScreenDocument::Backspace()
{
    throw std::logic_error("AltScreenDocument not yet implemented");
}

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
