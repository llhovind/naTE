#pragma once
#include "session/Connection.h"
#include <vector>

namespace term::session {

struct RestoreSession {
    Connection conn;
};

struct RestoreTile {
    std::vector<RestoreSession> sessions;
    int activeTabIndex = 0;
};

struct RestoreWindow {
    std::vector<RestoreTile> tiles;
    int x      = 0;
    int y      = 0;
    int width  = 1200;
    int height = 700;
};

struct RestoreState {
    int schemaVersion = 1;
    std::vector<RestoreWindow> windows;
};

} // namespace term::session
