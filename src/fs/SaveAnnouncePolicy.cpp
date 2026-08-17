#include "fs/SaveAnnouncePolicy.h"

namespace term::fs {

SaveAnnouncement SaveAnnouncePolicy::Decide(const transport::FsError& err,
                                            bool supersededByPending)
{
    if (supersededByPending)
        return SaveAnnouncement::Nothing;

    if (err.Failed()) {
        // Same failure twice running is the same broken state, not news.
        if (err.message == lastReported_)
            return SaveAnnouncement::Nothing;
        lastReported_ = err.message;
        return SaveAnnouncement::Failed;
    }

    // A save that lands re-arms reporting: whatever was wrong is over, and the
    // next thing to go wrong is news again even if it reads identically.
    lastReported_.clear();
    return SaveAnnouncement::Saved;
}

} // namespace term::fs
