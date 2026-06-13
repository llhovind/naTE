#pragma once

#include "db/IScrollbackRepository.h"
#include "session/RestoreState.h"
#include <vector>

namespace term::db {

// Deletes scrollback files whose UUIDs are not referenced by any RestoreState.
// Called once at startup (after loading all restore and workspace states) to
// clean up orphans left by crashes, partial restores, or workspace deletions.
void PurgeOrphanedScrollback(IScrollbackRepository& repo,
                              const std::vector<term::session::RestoreState>& allStates);

} // namespace term::db
