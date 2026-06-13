#include "transport/SshSession.h"

#include <libssh2.h>
#include <poll.h>

namespace term::transport::ssh {

bool PollUntilReady(_LIBSSH2_SESSION* session, int sockFd, int timeout_ms,
                    const std::atomic<bool>& running)
{
    const int dir = libssh2_session_block_directions(session);
    pollfd pfd{};
    pfd.fd     = sockFd;
    pfd.events = ((dir & LIBSSH2_SESSION_BLOCK_INBOUND)  ? POLLIN  : 0) |
                 ((dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) ? POLLOUT : 0);
    if (pfd.events == 0) pfd.events = POLLIN;
    ::poll(&pfd, 1, timeout_ms);
    return running.load();
}

std::string LastSshError(_LIBSSH2_SESSION* session)
{
    if (!session) return "(no session)";
    char* msg = nullptr;
    int   len = 0;
    libssh2_session_last_error(session, &msg, &len, 0);
    return msg ? std::string(msg, static_cast<size_t>(len)) : "(unknown)";
}

} // namespace term::transport::ssh
