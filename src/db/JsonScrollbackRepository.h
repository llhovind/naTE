#pragma once

#include "db/IScrollbackRepository.h"
#include <string>

namespace term::db {

// Stores scrollback data as plain NDJSON files under a flat directory.
//
// Three file patterns per UUID:
//   {uuid}-0.ndjson    — live segment (older, full)
//   {uuid}-1.ndjson    — live segment (current, being appended by ScrollbackWriter)
//   {uuid}.ndjson      — compacted clean-exit file (merged segments, plain NDJSON)
//
// Load prefers live segments (more recent after a crash) over the compacted file.
class JsonScrollbackRepository : public IScrollbackRepository {
public:
    explicit JsonScrollbackRepository(std::string dir);

    ScrollbackSnapshot Load(const std::string& uuid,
                                       size_t maxLines) const override;
    bool Exists(const std::string& uuid) const override;
    void Delete(const std::string& uuid) override;
    std::vector<std::string> ListAll() const override;

private:
    std::string m_dir;
};

} // namespace term::db
