#include "fs/FileMode.h"

namespace term::fs {

namespace {

char TypeChar(uint32_t mode)
{
    switch (mode & kFileTypeMask) {
        case kTypeDirectory: return 'd';
        case kTypeSymlink:   return 'l';
        case 0010000:        return 'p';  // FIFO
        case 0020000:        return 'c';  // character device
        case 0060000:        return 'b';  // block device
        case 0140000:        return 's';  // socket
        default:             return '-';
    }
}

} // namespace

std::string FormatPermissions(uint32_t mode)
{
    std::string out(10, '-');
    out[0] = TypeChar(mode);

    constexpr uint32_t bits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    constexpr char     chars[3] = {'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i)
        if (mode & bits[i]) out[static_cast<size_t>(1 + i)] = chars[i % 3];

    // The special bits overload the execute column of their triad: upper-case
    // when the execute bit is clear, lower-case when it is set.
    const auto overlay = [&out](size_t idx, bool special, char set, char unset) {
        if (!special) return;
        out[idx] = (out[idx] == 'x') ? set : unset;
    };
    overlay(3, (mode & kSetUid) != 0, 's', 'S');
    overlay(6, (mode & kSetGid) != 0, 's', 'S');
    overlay(9, (mode & kSticky) != 0, 't', 'T');

    return out;
}

std::string FormatOctal(uint32_t mode)
{
    const uint32_t perms = mode & kPermissionMask;
    std::string out(3, '0');
    out[0] = static_cast<char>('0' + ((perms >> 6) & 07));
    out[1] = static_cast<char>('0' + ((perms >> 3) & 07));
    out[2] = static_cast<char>('0' + (perms & 07));
    return out;
}

bool ParseOctal(const std::string& text, uint32_t& outMode)
{
    if (text.empty() || text.size() > 4) return false;

    uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '7') return false;
        value = (value << 3) | static_cast<uint32_t>(c - '0');
    }

    // Mask to permission plus setuid/setgid/sticky so file-type bits can never
    // be injected through this path.
    outMode = value & (kPermissionMask | kSpecialModeMask);
    return true;
}

} // namespace term::fs
