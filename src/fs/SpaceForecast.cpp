#include "fs/SpaceForecast.h"

#include <algorithm>

namespace term::fs {

SpaceForecast ForecastSpace(const std::vector<SpaceDemand>& demands,
                            std::optional<transport::FsSpaceInfo> destination,
                            std::optional<transport::FsSpaceInfo> staging)
{
    SpaceForecast forecast;

    for (const auto& demand : demands) {
        forecast.destination.requiredBytes += demand.bytes;
        if (demand.staged) {
            forecast.staging.requiredBytes =
                std::max(forecast.staging.requiredBytes, demand.bytes);
        }
    }

    if (destination) {
        forecast.destination.availableBytes = destination->availableBytes;
        forecast.destination.known          = true;
        forecast.destinationReadOnly        = destination->readOnly;
    }

    if (staging) {
        forecast.staging.availableBytes = staging->availableBytes;
        forecast.staging.known          = true;
    }

    return forecast;
}

} // namespace term::fs
