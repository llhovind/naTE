#pragma once
#include "session/Connection.h"
#include <ctime>
#include <string>

namespace term::db {

struct ConnectionProfile {
    std::string                   id;           // UUID v4
    std::string                   name;         // user-given display name
    term::session::TransportDesc  transport;    // variant<PtyDesc, LoopbackDesc, SshDesc, SerialDesc>
    bool                          wrapMode       = false;
    unsigned short                columnWidth    = 80;
    unsigned short                rows           = 24;
    std::time_t                   createdAt      = 0;
    std::time_t                   lastUsed       = 0;  // 0 = never used
    term::session::SessionInit    sessionInit;
    std::string                   profileTitle;
    bool                          useProfileTitle = false;
};

} // namespace term::db
