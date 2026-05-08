#pragma once
#include <cstddef>

struct SearchMatch {
    size_t lineIndex;  // absolute document line index
    size_t colStart;   // UTF-32 column (0-based)
    size_t colLen;     // match length in UTF-32 chars
};

namespace SearchHighlight {
    static constexpr int kMatchBg   = 226;  // xterm-256 bright yellow
    static constexpr int kCurrentBg = 208;  // xterm-256 orange
    static constexpr int kFg        = 0;    // black text on highlight
}
