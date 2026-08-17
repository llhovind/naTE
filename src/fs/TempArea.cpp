#include "fs/TempArea.h"

#include <cctype>
#include <cerrno>
#include <stdexcept>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace term::fs {

namespace {

// The only mode a temp directory of ours may end up with.
constexpr mode_t kPrivateDirMode = 0700;

// The bits that make an existing directory somebody else's business.
constexpr mode_t kSharedWriteBits = S_IWGRP | S_IWOTH;

} // namespace

bool EnsurePrivateDirectory(const std::string& path)
{
    if (::mkdir(path.c_str(), kPrivateDirMode) == 0)
        return true;

    // Already there is the ordinary case — every run after the first.
    if (errno != EEXIST)
        return false;

    struct stat st{};
    // lstat, not stat: a symlink left at this path by someone else must be
    // refused, not followed to wherever it points.
    if (::lstat(path.c_str(), &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return false;
    // Someone else got here first. Nothing about this directory can be trusted,
    // and there is no repair to make that would not be racing them for it.
    if (st.st_uid != ::geteuid())
        return false;

    // Ours, but possibly created before this rule existed, or by something that
    // let the umask decide. Tightened in place, because the owner is the one
    // party entitled to do that.
    if ((st.st_mode & 07777) != kPrivateDirMode) {
        if (::chmod(path.c_str(), kPrivateDirMode) != 0)
            // Could not be repaired. Whether that leaves it dangerous is not
            // worth guessing at: a directory others can write to is refused
            // outright, and a merely readable one is let through, since that is
            // where it already stood a moment ago.
            return (st.st_mode & kSharedWriteBits) == 0;
    }
    return true;
}

std::optional<int> OwnerPidOfTaggedName(const std::string& name)
{
    const auto sep = name.find(kOwnerSeparator);
    if (sep == std::string::npos || sep == 0)
        return std::nullopt;

    const std::string digits = name.substr(0, sep);
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return std::nullopt;
    }

    // A pid long enough to overflow was never written by this scheme, so the
    // name is not one of ours whatever else it is.
    try {
        return std::stoi(digits);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace term::fs
