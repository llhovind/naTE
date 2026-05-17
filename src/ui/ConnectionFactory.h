#pragma once
#include "session/Connection.h"
#include "ui/NewConnectionDialog.h"

namespace ui {

// Translates dialog output (ConnectionParams) into a domain Connection.
// labelIdx is embedded in auto-generated labels; supply 0 if you don't need it.
// The caller may override conn.label afterwards (e.g., from dlg.GetConnectionName()).
term::session::Connection ToConnection(const ConnectionParams& params, int labelIdx = 0);

} // namespace ui
