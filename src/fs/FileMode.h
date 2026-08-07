#pragma once
#include <cstdint>
#include <string>

// Pure helpers over POSIX file mode bits.
//
// These live here rather than in transport/ because formatting a mode for
// display is presentation logic, and rather than in ui/ because it is wx-free
// and unit-testable. The transport layer reports the raw mode; every consumer
// derives its own view from it, so there is exactly one source of truth.
namespace term::fs {

// POSIX file-type mask and the type values it selects. These are the spec, not
// magic numbers — they match S_IFMT and friends from <sys/stat.h>, spelled out
// here so this header stays portable and free of platform includes.
inline constexpr uint32_t kFileTypeMask = 0170000;
inline constexpr uint32_t kTypeDirectory = 0040000;
inline constexpr uint32_t kTypeSymlink   = 0120000;
inline constexpr uint32_t kTypeRegular   = 0100000;

// Mask selecting the nine rwxrwxrwx permission bits.
inline constexpr uint32_t kPermissionMask = 0777;

constexpr bool IsDirectory(uint32_t mode) noexcept
{
    return (mode & kFileTypeMask) == kTypeDirectory;
}

constexpr bool IsSymlink(uint32_t mode) noexcept
{
    return (mode & kFileTypeMask) == kTypeSymlink;
}

constexpr bool IsRegularFile(uint32_t mode) noexcept
{
    return (mode & kFileTypeMask) == kTypeRegular;
}

// Formats mode as a ten-character "drwxr-xr-x" string. setuid/setgid/sticky are
// rendered in the conventional s/s/t positions.
std::string FormatPermissions(uint32_t mode);

// Formats the low nine bits as a three-digit octal string ("644", "755").
std::string FormatOctal(uint32_t mode);

// Parses a one-to-four digit octal permission string. Returns false if the
// string is empty, over-long, or contains a non-octal digit; on success outMode
// receives the parsed value masked to the permission and setuid/setgid/sticky
// bits, so a caller can never smuggle file-type bits through a chmod dialog.
bool ParseOctal(const std::string& text, uint32_t& outMode);

} // namespace term::fs
