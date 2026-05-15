#pragma once

#include <span>
#include <wx/gdicmn.h>

#include "session/ISessionObserver.h"

namespace ui {

enum class DragIntent { Tabs, Tile };

// Pixels of motion required before a pending drag becomes an active drag.
// Shared by TabStrip and TerminalTile so both thresholds stay in sync.
inline constexpr int kDragThreshold = 5;

class ISessionDropTarget {
public:
    virtual ~ISessionDropTarget() = default;
    // Returns true if sessions were accepted; source should tear down on true.
    // screenPt is the drop position in screen coordinates; implementations
    // may use it for insertion-point logic (e.g. tab reorder).
    virtual bool DropSession(std::span<const term::session::SessionId> ids,
                             DragIntent intent,
                             wxPoint screenPt) = 0;
};

} // namespace ui
