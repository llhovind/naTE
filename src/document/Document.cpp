#include "document/Document.h"
#include <algorithm>
#include <stdexcept>

// ---------------------------------------------------------------------------
// DocLine
// ---------------------------------------------------------------------------

void DocLine::WriteAt(size_t col, char32_t ch)
{
    // --- pad: cursor jumped past end of line ---
    if (col > text.size()) {
        const size_t padStart = text.size();
        const size_t padCount = col - padStart;
        text.append(padCount, U' ');
        if (!styles.empty() && styles.back().style == Style{} &&
            styles.back().start + styles.back().length == padStart)
            styles.back().length += padCount;
        else
            styles.push_back({padStart, padCount, Style{}});
    }

    // --- append: cursor is at end of line ---
    if (col == text.size()) {
        text.push_back(ch);
        if (!styles.empty() && styles.back().style == currentStyle &&
            styles.back().start + styles.back().length == col)
            ++styles.back().length;
        else
            styles.push_back({col, 1, currentStyle});
        return;
    }

    // --- overwrite: cursor is within existing text ---
    text[col] = ch;
    for (size_t i = 0; i < styles.size(); ++i) {
        const size_t rStart = styles[i].start;
        const size_t rEnd   = rStart + styles[i].length;
        if (rStart > col || rEnd <= col) continue;

        if (styles[i].style == currentStyle) return;

        const Style  old       = styles[i].style;
        const size_t beforeLen = col - rStart;
        const size_t afterLen  = rEnd - col - 1;

        styles.erase(styles.begin() + i);

        size_t at = i;
        if (beforeLen > 0) styles.insert(styles.begin() + at++, {rStart,     beforeLen, old});
        styles.insert(styles.begin() + at++,                    {col,         1,         currentStyle});
        if (afterLen  > 0) styles.insert(styles.begin() + at,   {col + 1,   afterLen,  old});
        return;
    }
    // col not covered by any run (gap in style tracking)
    styles.push_back({col, 1, currentStyle});
}

void DocLine::Clear()
{
    text.clear();
    styles.clear();
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
    lines_.back().WriteAt(cursor_.col, ch);
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

void MainScreenDocument::CarriageReturn()
{
    cursor_.col = 0;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::SetCurrentStyle(const Style& style)
{
    lines_.back().currentStyle = style;
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

void MainScreenDocument::MoveCursorLeft(int n)
{
    cursor_.col = (cursor_.col >= static_cast<size_t>(n))
                    ? cursor_.col - static_cast<size_t>(n)
                    : 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorRight(int n)
{
    cursor_.col += static_cast<size_t>(n);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorUp(int n)
{
    cursor_.line = (cursor_.line >= static_cast<size_t>(n))
                    ? cursor_.line - static_cast<size_t>(n)
                    : 0;
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::MoveCursorDown(int n)
{
    const size_t lastLine = lines_.empty() ? 0 : lines_.size() - 1;
    cursor_.line = std::min(cursor_.line + static_cast<size_t>(n), lastLine);
    NotifyListeners(DocChangeType::CursorMove, cursor_.line);
}

void MainScreenDocument::EraseInLine(int mode)
{
    DocLine& line = lines_.back();
    switch (mode) {
    case 0: // cursor to end of line
        if (cursor_.col < line.text.size()) {
            line.text.erase(cursor_.col);
            for (auto it = line.styles.begin(); it != line.styles.end(); ) {
                if (it->start >= cursor_.col) {
                    it = line.styles.erase(it);
                } else if (it->start + it->length > cursor_.col) {
                    it->length = cursor_.col - it->start;
                    ++it;
                } else {
                    ++it;
                }
            }
        }
        break;
    case 1: // beginning of line to cursor (inclusive) — fill with spaces
        for (size_t i = 0; i <= cursor_.col && i < line.text.size(); ++i)
            line.text[i] = U' ';
        break;
    case 2: // entire line
        line.Clear();
        break;
    }
    NotifyListeners(DocChangeType::UpdateLine, cursor_.line);
}

// ---------------------------------------------------------------------------
// AltScreenDocument — stub until PtyTransport + alt-screen are implemented
// ---------------------------------------------------------------------------

void AltScreenDocument::CarriageReturn()
{
    throw std::logic_error("AltScreenDocument not yet implemented");
}

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

void AltScreenDocument::MoveCursorLeft(int)  { throw std::logic_error("AltScreenDocument not yet implemented"); }
void AltScreenDocument::MoveCursorRight(int) { throw std::logic_error("AltScreenDocument not yet implemented"); }
void AltScreenDocument::MoveCursorUp(int)    { throw std::logic_error("AltScreenDocument not yet implemented"); }
void AltScreenDocument::MoveCursorDown(int)  { throw std::logic_error("AltScreenDocument not yet implemented"); }
void AltScreenDocument::EraseInLine(int)     { throw std::logic_error("AltScreenDocument not yet implemented"); }
