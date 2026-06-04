#pragma once
#include <cstddef>

struct SearchMatch {
    size_t lineIndex;  // absolute document line index
    size_t colStart;   // UTF-32 column (0-based)
    size_t colLen;     // match length in UTF-32 chars
};
