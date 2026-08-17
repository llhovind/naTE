#include "fs/RelayWorkspace.h"
#include "fs/TempArea.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

#include <sys/statvfs.h>

namespace term::fs {

namespace {

// Leaf directory under the system temp path. Its own directory rather than
// loose files beside everyone else's: it bounds the sweep below to names this
// module wrote, and it makes the space a relay is using visible at a glance.
constexpr const char* kRelayDirName = "nate-relay";

// Longest source basename kept in a staging file's name. The name also carries
// a pid, a job id and a timestamp, and the whole thing has to stay inside
// NAME_MAX (255 on Linux) with room to spare for every one of those growing.
constexpr size_t kMaxLeafChars = 64;

// Reduces a remote basename to characters that are unambiguous in a local file
// name. Anything else becomes '_' rather than being escaped: this name is for a
// human reading `ls`, and nothing ever needs to recover the original from it.
std::string SanitiseLeaf(const std::string& leaf)
{
    std::string out;
    out.reserve(std::min(leaf.size(), kMaxLeafChars));

    for (char c : leaf) {
        if (out.size() >= kMaxLeafChars) break;
        const auto u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '.' || c == '_') ? c : '_';
    }
    return out;
}

} // namespace

std::string RelayRoot()
{
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    // temp_directory_path consults the environment and can fail if none of it
    // names a usable directory. /tmp is what it would have returned anyway on
    // the systems this runs on, and having a root is what keeps the caller from
    // needing an error path for something this trivial.
    if (ec) dir = "/tmp";

    return (dir / kRelayDirName).string();
}

bool EnsureRelayRoot()
{
    return EnsurePrivateDirectory(RelayRoot());
}

std::string MakeRelayPath(const std::string& leaf, uint64_t jobId,
                          int ownerPid, uint64_t stamp)
{
    const std::string name = std::to_string(ownerPid) + kOwnerSeparator +
                             std::to_string(jobId) + kOwnerSeparator +
                             std::to_string(stamp) + kOwnerSeparator +
                             SanitiseLeaf(leaf);

    return (std::filesystem::path(RelayRoot()) / name).string();
}

std::optional<transport::FsSpaceInfo> RelayVolumeSpace()
{
    // The root, not a file in it: before the first transfer of a run there is
    // nothing in there to ask about, and the volume is the same either way.
    // When the root itself is not there yet, the temp directory above it is on
    // that same volume and answers identically — which is what makes the very
    // first transfer of a run checkable rather than unmeasured.
    struct statvfs vfs{};
    if (::statvfs(RelayRoot().c_str(), &vfs) != 0) {
        std::error_code ec;
        auto fallback = std::filesystem::temp_directory_path(ec);
        if (ec || ::statvfs(fallback.c_str(), &vfs) != 0)
            return std::nullopt;
    }

    transport::FsSpaceInfo info;
    info.totalBytes     = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    info.availableBytes = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    info.readOnly       = (vfs.f_flag & ST_RDONLY) != 0;
    return info;
}

size_t PurgeOrphanedRelayFiles(const OwnerIsLiveFn& isLive)
{
    const std::filesystem::path root(RelayRoot());

    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec))
        return 0;

    // Collected first rather than removed during the walk: deleting entries out
    // from under a directory iterator is undefined.
    std::vector<std::filesystem::path> orphans;

    std::filesystem::directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return 0;

    for (const auto& entry : it) {
        std::error_code fileEc;
        // Only regular files. A directory here was not written by this module,
        // and recursing into one would take the sweep somewhere it has no
        // grounds to be deleting things.
        if (!entry.is_regular_file(fileEc)) continue;

        const auto owner = OwnerPidOfTaggedName(entry.path().filename().string());
        if (!owner) continue;          // not a staging file: not ours to judge
        if (isLive(*owner)) continue;  // another instance is relaying through it

        orphans.push_back(entry.path());
    }

    size_t reclaimed = 0;
    for (const auto& file : orphans) {
        std::error_code rmEc;
        if (std::filesystem::remove(file, rmEc) && !rmEc)
            ++reclaimed;
    }
    return reclaimed;
}

} // namespace term::fs
