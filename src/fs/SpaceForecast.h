#pragma once
#include "transport/IRemoteFileSystem.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace term::fs {

// Whether a set of queued transfers will fit where they are going.
//
// Pure arithmetic over sizes already known — no I/O, no filesystem, no queue.
// The two volumes involved are loaded by quite different rules and getting
// either wrong produces a warning nobody can act on, so the rules live here on
// their own where they can be stated and tested directly.

// What one queued transfer asks of the volumes it touches.
struct SpaceDemand {
    uint64_t bytes = 0;
    // True when the bytes pass through this machine's staging area on the way —
    // a copy between two remote hosts, which SFTP cannot do server to server.
    bool     staged = false;
};

// One volume: what it was asked for against what it has.
struct VolumeForecast {
    uint64_t requiredBytes  = 0;
    uint64_t availableBytes = 0;

    // False when the volume could not be measured — an SFTP server without the
    // statvfs extension, or a query that simply failed. Nothing may be
    // concluded from availableBytes then, and Short() stays false: a check that
    // warned about what it could not measure would cry wolf on every server
    // lacking an optional protocol extension.
    bool known = false;

    bool Short() const noexcept
    {
        return known && requiredBytes > availableBytes;
    }

    // How much more room is needed; zero unless Short().
    uint64_t ShortfallBytes() const noexcept
    {
        return Short() ? requiredBytes - availableBytes : 0;
    }
};

// The size at which not being able to check stops being acceptable.
//
// An unmeasurable destination is normally not worth interrupting for: SFTP's
// statvfs is an optional extension, and a server without it would otherwise
// prompt on every small copy until the user stopped reading prompts entirely.
// That reasoning holds only while being wrong is cheap. Past this point it is
// not — the copy runs for hours, fills a volume somebody else may depend on,
// and the user had no way to know it was never verified. "I could not check
// this" is then worth saying, and the choice is still theirs.
inline constexpr uint64_t kUnverifiableWarningBytes = 1ULL << 30;   // 1 GiB

struct SpaceForecast {
    VolumeForecast destination;
    // This machine's staging area. Zero required, and never short, when nothing
    // in the batch is staged.
    VolumeForecast staging;

    // The destination volume is mounted read-only. Independent of space: a
    // read-only volume with room to spare still cannot take the transfer.
    bool destinationReadOnly = false;

    bool Short() const noexcept
    {
        return destination.Short() || staging.Short();
    }

    // A batch large enough to matter, going somewhere that could not be
    // measured at all.
    //
    // This exists because silence about an unmeasured volume is indis-
    // tinguishable from approval. Reporting "unknown" only in a status line
    // meant a 4 TiB copy onto a destination nobody could measure started with
    // no more ceremony than one that had been checked and fitted.
    bool DestinationUnverifiable() const noexcept
    {
        return !destination.known &&
               destination.requiredBytes >= kUnverifiableWarningBytes;
    }

    // Whether there is anything worth stopping to tell the user about.
    bool Concerning() const noexcept
    {
        return Short() || destinationReadOnly || DestinationUnverifiable();
    }
};

// Totals a batch onto the two volumes.
//
// The two are loaded differently, and the difference is the whole point:
//
//   - The destination takes the **sum**. Every byte of every job lands there
//     and stays there.
//   - Staging takes the **maximum** of the staged jobs, not the sum. The queue
//     runs one job at a time and deletes each staging file as that job retires,
//     so what is in use at any instant is a single file — the largest one. A
//     sum here would refuse a batch that fits perfectly well, and on a /tmp
//     sized to a fraction of RAM it would refuse almost every batch.
//
// A volume passed as nullopt is unmeasured and reported as such rather than
// assumed empty or assumed infinite.
SpaceForecast ForecastSpace(const std::vector<SpaceDemand>& demands,
                            std::optional<transport::FsSpaceInfo> destination,
                            std::optional<transport::FsSpaceInfo> staging);

} // namespace term::fs
