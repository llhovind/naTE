#pragma once

#include <string>
#include <vector>
#include <deque>
#include <cstdint>

struct Style {
    int fg = 37;   // placeholder (e.g. ANSI color)
    int bg = 40;
    bool bold = false;

    bool operator==(const Style& other) const {
        return fg == other.fg && bg == other.bg && bold == other.bold;
    }

    bool operator!=(const Style& other) const {
        return !(*this == other);
    }
};

struct StyleRun {
    size_t start;
    size_t length;
    Style style;
};

struct DocLine {
    std::u32string text;
    std::vector<StyleRun> styles;
    Style currentStyle;

    void AppendChar(char32_t ch);
};

class Document {
public:
    void AppendChar(char32_t ch);
    void NewLine();
    void SetCurrentStyle(const Style& style);

    const std::deque<DocLine>& GetLines() const { return lines; }

private:
    std::deque<DocLine> lines;
};