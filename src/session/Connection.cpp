#include "session/Connection.h"

namespace term::session {

std::vector<Connection> Connection::Defaults(const std::string& shell)
{
    return {
        Connection{ "Local Shell", PtyDesc{ shell } },
        Connection{ "Loopback",    LoopbackDesc{} },
    };
}

} // namespace term::session
