#include <catch2/catch_test_macros.hpp>

#include "fs/SpaceForecast.h"

using namespace term::fs;
using term::transport::FsSpaceInfo;

namespace {

FsSpaceInfo Volume(uint64_t available, bool readOnly = false)
{
    FsSpaceInfo info;
    info.totalBytes     = available * 2;   // capacity is not what any rule turns on
    info.availableBytes = available;
    info.readOnly       = readOnly;
    return info;
}

constexpr uint64_t kGb = 1024ULL * 1024 * 1024;

} // namespace

// ---------------------------------------------------------------------------
// Destination: the sum
// ---------------------------------------------------------------------------

TEST_CASE("given several transfers when forecast then the destination is charged their total") {
    const auto forecast = ForecastSpace({{100, false}, {250, false}, {50, false}},
                                        Volume(1000), std::nullopt);

    REQUIRE(forecast.destination.requiredBytes == 400);
    REQUIRE(forecast.destination.availableBytes == 1000);
    REQUIRE(forecast.destination.known);
    REQUIRE_FALSE(forecast.destination.Short());
    REQUIRE_FALSE(forecast.Short());
}

TEST_CASE("given transfers larger than the destination when forecast then it is short by the difference") {
    const auto forecast = ForecastSpace({{600, false}, {600, false}},
                                        Volume(1000), std::nullopt);

    REQUIRE(forecast.destination.Short());
    REQUIRE(forecast.destination.ShortfallBytes() == 200);
    REQUIRE(forecast.Short());
    REQUIRE(forecast.Concerning());
}

TEST_CASE("given a batch exactly filling the destination when forecast then it is not short") {
    // Fitting exactly is fitting. An off-by-one here would warn on every
    // transfer that happens to land on a round number.
    const auto forecast = ForecastSpace({{1000, false}}, Volume(1000), std::nullopt);

    REQUIRE_FALSE(forecast.destination.Short());
    REQUIRE(forecast.destination.ShortfallBytes() == 0);
}

// ---------------------------------------------------------------------------
// Staging: the maximum, not the sum
// ---------------------------------------------------------------------------

TEST_CASE("given several staged transfers when forecast then staging is charged only the largest") {
    // The queue runs one job at a time and reclaims each staging file as that
    // job retires, so what is in use at any instant is a single file. Summing
    // here would refuse batches that fit perfectly well.
    const auto forecast = ForecastSpace({{100, true}, {700, true}, {300, true}},
                                        Volume(kGb), Volume(1000));

    REQUIRE(forecast.staging.requiredBytes == 700);
    REQUIRE_FALSE(forecast.staging.Short());
    REQUIRE(forecast.destination.requiredBytes == 1100);
}

TEST_CASE("given a staged file larger than the staging volume when forecast then staging is short") {
    const auto forecast = ForecastSpace({{5000, true}}, Volume(kGb), Volume(1000));

    REQUIRE(forecast.staging.Short());
    REQUIRE(forecast.staging.ShortfallBytes() == 4000);
    REQUIRE(forecast.Short());
    // The destination is roomy; only staging is the problem.
    REQUIRE_FALSE(forecast.destination.Short());
}

TEST_CASE("given no staged transfers when forecast then staging is charged nothing") {
    const auto forecast = ForecastSpace({{5000, false}, {5000, false}},
                                        Volume(kGb), Volume(10));

    REQUIRE(forecast.staging.requiredBytes == 0);
    REQUIRE_FALSE(forecast.staging.Short());
}

TEST_CASE("given a mixed batch when forecast then only staged jobs load the staging volume") {
    const auto forecast = ForecastSpace({{900, false}, {400, true}, {100, false}},
                                        Volume(kGb), Volume(500));

    REQUIRE(forecast.destination.requiredBytes == 1400);
    REQUIRE(forecast.staging.requiredBytes == 400);
    REQUIRE_FALSE(forecast.Short());
}

// ---------------------------------------------------------------------------
// Unmeasured volumes
// ---------------------------------------------------------------------------

TEST_CASE("given a small batch to a destination that cannot report space then it passes unremarked") {
    // An SFTP server without statvfs@openssh.com. Warning about every small
    // copy to such a server trains the user to dismiss the dialog unread, and
    // reporting zero would be an outright lie.
    const auto forecast = ForecastSpace({{1024, false}}, std::nullopt, std::nullopt);

    REQUIRE_FALSE(forecast.destination.known);
    REQUIRE_FALSE(forecast.destination.Short());
    REQUIRE_FALSE(forecast.DestinationUnverifiable());
    REQUIRE_FALSE(forecast.Concerning());
}

TEST_CASE("given a large batch to a destination that cannot report space then it is flagged as unchecked") {
    // The regression this exists for. Staying silent about an unmeasured volume
    // made a 4 TiB copy start with no more ceremony than one that had been
    // checked and fitted — silence was indistinguishable from approval.
    const auto forecast = ForecastSpace({{kGb * 100, false}}, std::nullopt, std::nullopt);

    REQUIRE_FALSE(forecast.destination.known);
    REQUIRE(forecast.DestinationUnverifiable());
    REQUIRE(forecast.Concerning());
    // Still not a claim that it will not fit — nothing is known either way.
    REQUIRE_FALSE(forecast.Short());
    // The requirement is known even when the capacity is not.
    REQUIRE(forecast.destination.requiredBytes == kGb * 100);
}

TEST_CASE("given an unmeasured destination at the threshold then the boundary is inclusive") {
    const auto under = ForecastSpace({{kUnverifiableWarningBytes - 1, false}},
                                     std::nullopt, std::nullopt);
    const auto at    = ForecastSpace({{kUnverifiableWarningBytes, false}},
                                     std::nullopt, std::nullopt);

    REQUIRE_FALSE(under.DestinationUnverifiable());
    REQUIRE(at.DestinationUnverifiable());
}

TEST_CASE("given a measured destination with room when large then it is not flagged as unchecked") {
    // Unverifiable is about the absence of an answer, not about size. A volume
    // that answered and has room must not be dragged into a warning.
    const auto forecast = ForecastSpace({{kGb * 100, false}}, Volume(kGb * 500),
                                        std::nullopt);

    REQUIRE_FALSE(forecast.DestinationUnverifiable());
    REQUIRE_FALSE(forecast.Concerning());
}

TEST_CASE("given an unmeasured staging volume when the destination is short then the destination still warns") {
    const auto forecast = ForecastSpace({{5000, true}}, Volume(100), std::nullopt);

    REQUIRE(forecast.destination.Short());
    REQUIRE_FALSE(forecast.staging.known);
    REQUIRE_FALSE(forecast.staging.Short());
    REQUIRE(forecast.Short());
}

// ---------------------------------------------------------------------------
// Read-only
// ---------------------------------------------------------------------------

TEST_CASE("given a read-only destination with room to spare when forecast then it is still concerning") {
    // No amount of free space makes a read-only volume writable, so this has to
    // reach the user even though nothing is short.
    const auto forecast = ForecastSpace({{10, false}}, Volume(kGb, true), std::nullopt);

    REQUIRE(forecast.destinationReadOnly);
    REQUIRE_FALSE(forecast.Short());
    REQUIRE(forecast.Concerning());
}

TEST_CASE("given an empty batch when forecast then nothing is required or concerning") {
    const auto forecast = ForecastSpace({}, Volume(1000), Volume(1000));

    REQUIRE(forecast.destination.requiredBytes == 0);
    REQUIRE(forecast.staging.requiredBytes == 0);
    REQUIRE_FALSE(forecast.Concerning());
}
