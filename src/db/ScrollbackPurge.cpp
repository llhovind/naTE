#include "db/ScrollbackPurge.h"
#include <unordered_set>

namespace term::db {

void PurgeOrphanedScrollback(IScrollbackRepository& repo,
                              const std::vector<term::session::RestoreState>& allStates)
{
    // Collect every UUID referenced by any session in any state.
    std::unordered_set<std::string> referenced;
    for (const auto& state : allStates)
        for (const auto& window : state.windows)
            for (const auto& tile : window.tiles)
                for (const auto& session : tile.sessions)
                    if (!session.scrollbackUuid.empty())
                        referenced.insert(session.scrollbackUuid);

    // Delete any scrollback file whose UUID is not referenced.
    for (const auto& uuid : repo.ListAll())
        if (referenced.find(uuid) == referenced.end())
            repo.Delete(uuid);
}

} // namespace term::db
