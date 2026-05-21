#include "ui/UrlScanner.h"

namespace UrlScanner {

namespace {

// Recognised URL scheme prefixes (lower-case for matching).
constexpr const char32_t* kPrefixes[] = {
    U"https://",
    U"http://",
    U"ftp://",
    U"mailto:",
};

// Returns true if cp is a legal URL character per RFC 3986 unreserved +
// reserved sets, minus characters that are unambiguously terminal punctuation
// when they appear at the end of a URL in prose (handled by TrimTrailing).
bool IsUrlChar(char32_t cp)
{
    // Whitespace and control characters always terminate.
    if (cp <= 0x20 || cp == 0x7F) return false;

    // Characters that never appear in a well-formed URL.
    switch (cp) {
        case U'"': case U'\'': case U'`':
        case U'\\': case U'<': case U'>':
        case U' ':
            return false;
        default:
            break;
    }
    return true;
}

// Characters that are legal inside a URL but are almost always trailing prose
// punctuation when they appear at the very end of the matched span.
bool IsTrailingPunct(char32_t cp)
{
    switch (cp) {
        case U'.': case U',': case U';': case U'!': case U'?':
        case U':': case U')': case U']':
            return true;
        default:
            return false;
    }
}

size_t PrefixLen(const char32_t* prefix)
{
    size_t n = 0;
    while (prefix[n]) ++n;
    return n;
}

// Case-insensitive comparison of text[pos..pos+plen) against prefix.
bool MatchPrefix(const std::u32string& text, size_t pos, const char32_t* prefix, size_t plen)
{
    if (pos + plen > text.size()) return false;
    for (size_t i = 0; i < plen; ++i) {
        char32_t a = text[pos + i];
        char32_t b = prefix[i];
        // Lower-case a for comparison (ASCII range only — schemes are ASCII).
        if (a >= U'A' && a <= U'Z') a += 32;
        if (a != b) return false;
    }
    return true;
}

}  // namespace

std::vector<UrlSpan> ScanLine(const std::u32string& text)
{
    std::vector<UrlSpan> result;
    const size_t n = text.size();

    size_t i = 0;
    while (i < n) {
        // Try each prefix at position i.
        bool found = false;
        for (const char32_t* pfx : kPrefixes) {
            const size_t plen = PrefixLen(pfx);
            if (!MatchPrefix(text, i, pfx, plen)) continue;

            // Extend span forward while characters are valid URL chars.
            size_t end = i + plen;
            while (end < n && IsUrlChar(text[end]))
                ++end;

            // Trim trailing punctuation that is likely prose, not URL.
            while (end > i + plen && IsTrailingPunct(text[end - 1]))
                --end;

            // Only record if span has content beyond the scheme itself.
            if (end > i + plen) {
                UrlSpan span;
                span.col = i;
                span.len = end - i;
                span.url = text.substr(i, span.len);
                result.push_back(std::move(span));
                i = end;
                found = true;
            }
            break;
        }

        if (!found) ++i;
    }

    return result;
}

}  // namespace UrlScanner
