#pragma once

#include <string>
#include <utility>

// Pure-std word-boundary finder used by double-click selection.
// No wx, no document types — fully unit-testable headlessly.
namespace WordSelector {

// Scan `lineText` (UTF-32) for a token matching `regexPattern` that contains
// `clickCol`. Returns {startCol, endCol} as a half-open UTF-32 column range.
// Returns {clickCol, clickCol} (zero-length) when no match covers the position.
// If `regexPattern` is invalid, falls back to "[^\\s]+" without throwing.
std::pair<int, int> FindWordBounds(const std::u32string& lineText,
                                   int                   clickCol,
                                   const std::string&    regexPattern);

} // namespace WordSelector
