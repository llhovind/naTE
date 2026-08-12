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

// setuid/setgid/sticky, in the order they modify the x column of each triad,
// and the mask selecting all three. Public rather than file-private because a
// chmod path has to preserve them, and a caller that cannot name them ends up
// respelling 07000 by hand.
inline constexpr uint32_t kSetUid = 04000;
inline constexpr uint32_t kSetGid = 02000;
inline constexpr uint32_t kSticky = 01000;
inline constexpr uint32_t kSpecialModeMask = kSetUid | kSetGid | kSticky;

// Permissions for a directory naTE creates — whether the user asked for it in
// the explorer or a recursive copy needed it on the way. rwxr-xr-x is what
// mkdir(1) produces under the conventional 022 umask, so a directory made here
// is indistinguishable from one the user made at a shell. Servers apply their
// own umask on top, which is theirs to decide.
inline constexpr uint32_t kDefaultDirectoryMode = 0755;

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
