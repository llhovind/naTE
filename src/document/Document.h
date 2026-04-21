#pragma once

#include <string>
#include <vector>
#include <deque>
#include <cstdint>

struct Style {
    int fg = 37;
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
    virtual ~Document() = default;

    virtual void AppendChar(char32_t ch) = 0;
    virtual void NewLine() = 0;
    virtual void SetCurrentStyle(const Style& style) = 0;
    virtual const std::deque<DocLine>& GetLines() const = 0;
};

class MainScreenDocument : public Document {
public:
    void AppendChar(char32_t ch) override;
    void NewLine() override;
    void SetCurrentStyle(const Style& style) override;
    const std::deque<DocLine>& GetLines() const override { return lines_; }

private:
    std::deque<DocLine> lines_;
};

class AltScreenDocument : public Document {
public:
    void AppendChar(char32_t ch) override;
    void NewLine() override;
    void SetCurrentStyle(const Style& style) override;
    const std::deque<DocLine>& GetLines() const override;
};
