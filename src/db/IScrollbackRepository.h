#pragma once

#include "document/Document.h"
#include <string>
#include <vector>

namespace term::db {

class IScrollbackRepository {
public:
    virtual ~IScrollbackRepository() = default;

    // Loads scrollback for the given UUID.
    // Prefers live segment files (crash-recovery path) over the compacted .ndjson
    // (clean-exit path). Returns the last maxLines lines from the merged content.
    virtual ScrollbackSnapshot Load(const std::string& uuid,
                                               size_t maxLines) const = 0;

    // Returns true if any scrollback file exists for the given UUID.
    virtual bool Exists(const std::string& uuid) const = 0;

    // Removes all scrollback files for the given UUID.
    virtual void Delete(const std::string& uuid) = 0;

    // Returns all UUIDs that have at least one scrollback file present.
    // Used by the startup orphan sweep.
    virtual std::vector<std::string> ListAll() const = 0;
};

} // namespace term::db
