#pragma once
#include <atomic>
#include <string>

struct _LIBSSH2_SESSION;

namespace term::transport::ssh {

// Poll timeout shared by every non-blocking libssh2 EAGAIN wait loop.
inline constexpr int kPollTimeoutMs = 100;

// Blocks up to timeout_ms until the session socket is ready in the direction
// libssh2 last requested (falls back to POLLIN when it requested neither).
// Returns running.load() so callers can break their EAGAIN loop the instant
// the transport is asked to shut down.
bool PollUntilReady(_LIBSSH2_SESSION* session, int sockFd, int timeout_ms,
                    const std::atomic<bool>& running);

// Returns the last libssh2 error string for session, or a placeholder when
// the session is null / no message is available.
std::string LastSshError(_LIBSSH2_SESSION* session);

} // namespace term::transport::ssh
