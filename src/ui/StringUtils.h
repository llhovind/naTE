#pragma once
#include <string>
#include <wx/string.h>

// Encode a UTF-32 string as UTF-8.
inline std::string ToUtf8(const std::u32string& u32)
{
    std::string utf8;
    utf8.reserve(u32.size());
    for (char32_t cp : u32) {
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
    return utf8;
}

inline wxString ToWxString(const std::u32string& u32)
{
    return wxString::FromUTF8(ToUtf8(u32));
}

// Decodes bytes that came from outside naTE — remote filenames in particular —
// for display.
//
// Remote filesystems store names as opaque bytes, so a latin-1 or otherwise
// non-UTF-8 name is entirely legal. wxString::FromUTF8 returns an *empty*
// string for such input, which would make the file silently vanish from a
// listing rather than merely look wrong. Falling back to a byte-wise latin-1
// read keeps it visible and selectable; the caller must still use the original
// bytes for any path it sends back to the server.
inline wxString DecodeForDisplay(const std::string& bytes)
{
    if (bytes.empty()) return {};
    if (wxString decoded = wxString::FromUTF8(bytes); !decoded.empty())
        return decoded;
    return wxString::From8BitData(bytes.data(), bytes.size());
}
