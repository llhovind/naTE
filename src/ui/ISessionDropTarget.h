#pragma once

#include <span>

#include "session/ISessionObserver.h"

namespace ui {

enum class DragIntent { Tabs, Tile };

class ISessionDropTarget {
public:
    virtual ~ISessionDropTarget() = default;
    // Returns true if sessions were accepted; source should tear down on true.
    virtual bool DropSession(std::span<const term::session::SessionId> ids,
                             DragIntent intent) = 0;
};

} // namespace ui
