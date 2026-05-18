#pragma once
#include "session/RestoreState.h"

namespace term::db {

class ISessionRestoreRepository {
public:
    virtual ~ISessionRestoreRepository() = default;

    // Returns true iff a persisted snapshot exists and contains at least one session.
    virtual bool HasSnapshot() const = 0;

    // Loads the persisted snapshot. Caller should check HasSnapshot() first.
    virtual term::session::RestoreState Load() const = 0;

    // Persists the given state atomically. If state contains no windows, calls Delete().
    virtual void Save(const term::session::RestoreState& state) = 0;

    // Removes the persisted snapshot file.
    virtual void Delete() = 0;
};

} // namespace term::db
