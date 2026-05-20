#pragma once
#include "db/ConnectionStore.h"
#include "session/Connection.h"
#include "ui/NewConnectionDialog.h"

namespace ui {

// Translates dialog output (ConnectionParams) into a domain Connection.
// labelIdx is embedded in auto-generated labels; supply 0 if you don't need it.
// The caller may override conn.label afterwards (e.g., from dlg.GetConnectionName()).
term::session::Connection ToConnection(const ConnectionParams& params, int labelIdx = 0);

// Persists conn to store. If existingId is non-empty, the matching profile is
// updated in-place (preserving id/createdAt/lastUsed); otherwise a new profile
// is added. name is used as the stored profile name.
void SaveProfile(term::db::ConnectionStore& store,
                 const term::session::Connection& conn,
                 const std::string& name,
                 const std::string& existingId = {});

} // namespace ui
