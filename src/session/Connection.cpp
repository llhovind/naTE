#include "session/Connection.h"

namespace term::session {

std::vector<Connection> Connection::Defaults(const std::string& shell)
{
    return {
        Connection{ "Local Shell",    transport::PtyDesc{ shell } },
        Connection{ "Loopback",       transport::LoopbackDesc{} },
        Connection{ "Serial Console", transport::SerialDesc{ "/dev/ttyUSB0", 115200 } },
    };
}

} // namespace term::session
