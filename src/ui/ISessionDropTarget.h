#pragma once

#include <span>

#include "session/ISessionObserver.h"

namespace ui {

class ISessionDropTarget {
public:
    virtual ~ISessionDropTarget() = default;
    // Returns true if sessions were accepted; source should tear down on true.
    virtual bool DropSession(std::span<const term::session::SessionId> ids) = 0;
};

} // namespace ui
