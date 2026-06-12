#pragma once
#include <cstddef>
#include <string>

struct SearchMatch {
    size_t lineIndex;  // absolute document line index
    size_t colStart;   // UTF-32 column (0-based)
    size_t colLen;     // match length in UTF-32 chars
};

// ASCII-only case fold shared by DocLayout (haystack side) and
// SearchController (needle side) so both sides agree character-for-character.
inline char32_t SearchCaseFold(char32_t c)
{
    return (c >= U'A' && c <= U'Z') ? c + (U'a' - U'A') : c;
}

inline std::u32string SearchCaseFoldStr(const std::u32string& s)
{
    std::u32string r;
    r.reserve(s.size());
    for (char32_t c : s)
        r.push_back(SearchCaseFold(c));
    return r;
}
