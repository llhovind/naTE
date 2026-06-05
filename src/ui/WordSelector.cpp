#include "ui/WordSelector.h"

#include "document/Document.h"  // kWideFiller

#include <regex>
#include <string>
#include <vector>

namespace WordSelector {

namespace {

// Transcode UTF-32 to UTF-8, building a parallel map from UTF-32 column index
// to the byte offset of that character in the resulting UTF-8 string.
// kWideFiller sentinels are skipped (they are not real characters).
std::string BuildUtf8WithMap(const std::u32string& u32,
                             std::vector<int>&     colToByteOffset)
{
    std::string utf8;
    utf8.reserve(u32.size());
    colToByteOffset.reserve(u32.size());

    for (int col = 0; col < static_cast<int>(u32.size()); ++col) {
        const char32_t cp = u32[static_cast<size_t>(col)];

        if (cp == kWideFiller) {
            // Filler for the second cell of a wide character: not a real code
            // point, so map it to the same byte offset as the previous char.
            const int prev = colToByteOffset.empty()
                           ? 0
                           : colToByteOffset.back();
            colToByteOffset.push_back(prev);
            continue;
        }

        colToByteOffset.push_back(static_cast<int>(utf8.size()));

        if (cp < 0x80) {
            utf8.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | ((cp >> 6)  & 0x3F)));
            utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Sentinel: end-of-string byte offset for the reverse lookup.
    colToByteOffset.push_back(static_cast<int>(utf8.size()));

    return utf8;
}

// Given a UTF-8 byte offset `byteOffset`, find the UTF-32 column index using
// the colToByteOffset map (binary search).
// The sentinel entry (colToByteOffset[u32.size()] = utf8.size()) is included
// so that a matchEnd pointing one past the last character returns the correct
// exclusive end column.
int ByteOffsetToCol(const std::vector<int>& colToByteOffset, int byteOffset)
{
    // The map has u32.size()+1 entries; entry [i] is the byte start of col i.
    // Find the last col whose byte start <= byteOffset.
    int lo = 0;
    int hi = static_cast<int>(colToByteOffset.size()) - 1; // include sentinel
    while (lo < hi) {
        const int mid = lo + (hi - lo + 1) / 2;
        if (colToByteOffset[static_cast<size_t>(mid)] <= byteOffset)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

const std::string kFallbackPattern = "[^\\s]+";

} // namespace

std::pair<int, int> FindWordBounds(const std::u32string& lineText,
                                   int                   clickCol,
                                   const std::string&    regexPattern)
{
    const std::pair<int, int> kNoMatch{clickCol, clickCol};

    if (lineText.empty() ||
        clickCol < 0 ||
        clickCol >= static_cast<int>(lineText.size()))
        return kNoMatch;

    // Build UTF-8 representation with column→byte map.
    std::vector<int> colToByteOffset;
    const std::string utf8 = BuildUtf8WithMap(lineText, colToByteOffset);

    const int clickByte = colToByteOffset[static_cast<size_t>(clickCol)];

    // Compile the pattern, falling back on any error.
    std::regex re;
    try {
        re = std::regex(regexPattern);
    } catch (const std::regex_error&) {
        try {
            re = std::regex(kFallbackPattern);
        } catch (...) {
            return kNoMatch;
        }
    }

    // Iterate all matches and find the one that contains clickByte.
    auto it  = std::sregex_iterator(utf8.cbegin(), utf8.cend(), re);
    auto end = std::sregex_iterator{};
    for (; it != end; ++it) {
        const auto& m = *it;
        const int matchStart = static_cast<int>(m.position());
        const int matchEnd   = matchStart + static_cast<int>(m.length());

        // Half-open interval: [matchStart, matchEnd)
        if (matchStart <= clickByte && clickByte < matchEnd) {
            const int startCol = ByteOffsetToCol(colToByteOffset, matchStart);
            const int endCol   = ByteOffsetToCol(colToByteOffset, matchEnd);
            return {startCol, endCol};
        }
    }

    return kNoMatch;
}

} // namespace WordSelector
