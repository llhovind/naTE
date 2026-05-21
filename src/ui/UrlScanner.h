#pragma once

#include <string>
#include <vector>

namespace UrlScanner {

struct UrlSpan {
    size_t         col;  // start column (UTF-32 code-point index in the line)
    size_t         len;  // length in code points
    std::u32string url;  // the matched URL text
};

// Scan one terminal line for URL spans.
// Recognises http://, https://, ftp://, and mailto: prefixes.
// Returns spans in ascending col order.
std::vector<UrlSpan> ScanLine(const std::u32string& text);

}  // namespace UrlScanner
