#pragma once

#include <string>
#include <vector>
#include <deque>
#include <cstdint>
#include "document/IDocumentListener.h"

struct Style {
    int fg = -1;  // -1 = use terminal default
    int bg = -1;  // -1 = use terminal default
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

struct CursorPos {
    size_t line{0};
    size_t col{0};
};

struct DocLine {
    std::u32string text;
    std::vector<StyleRun> styles;
    Style currentStyle;

    void AppendInsertChar(char32_t ch);
};

class Document {
public:
    virtual ~Document() = default;

    virtual void AppendInsertChar(char32_t ch) = 0;
    virtual void NewLine() = 0;
    virtual void SetCurrentStyle(const Style& style) = 0;
    virtual const std::deque<DocLine>& GetLines() const = 0;

    CursorPos GetCursor() const { return cursor_; }

    void AddListener(IDocumentListener* listener);
    void RemoveListener(IDocumentListener* listener);

protected:
    void NotifyListeners(DocChangeType type, size_t lineIndex);

    CursorPos cursor_{};

private:
    std::vector<IDocumentListener*> listeners_;
};

class MainScreenDocument : public Document {
public:
    MainScreenDocument();

    void AppendInsertChar(char32_t ch) override;
    void NewLine() override;
    void SetCurrentStyle(const Style& style) override;
    const std::deque<DocLine>& GetLines() const override { return lines_; }

private:
    std::deque<DocLine> lines_;
};

class AltScreenDocument : public Document {
public:
    void AppendInsertChar(char32_t ch) override;
    void NewLine() override;
    void SetCurrentStyle(const Style& style) override;
    const std::deque<DocLine>& GetLines() const override;
};
