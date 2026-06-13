#pragma once
#include "transport/TransportDesc.h"    // SshDesc, SshAuthMethod
#include "transport/TransportError.h"   // TransportError::Category

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_AGENT;

namespace term::transport {

class ITransportTarget;

// Runs the configured SSH authentication method on a non-blocking session.
// Pure policy: it never writes to the terminal or fires transport callbacks —
// it returns a typed Result and the caller (SshTransport) decides how to
// surface a failure. The keyboard-interactive path delegates prompts to the
// ITransportTarget via the libssh2 session abstract pointer, which it sets to
// `this` for the duration of that one call.
//
// session_, sockFd_ and agent_ are bound by reference: the transport creates
// the session/socket on the worker thread after this object is constructed,
// and owns the agent handle's teardown.
class SshAuthenticator {
public:
    struct Result {
        bool                     ok       = false;
        TransportError::Category category = TransportError::Category::Authentication;
        std::string              message;   // empty => transport shutting down (no user-facing error)
    };

    SshAuthenticator(const SshDesc& desc, _LIBSSH2_SESSION*& session, int& sockFd,
                     const std::atomic<bool>& running, _LIBSSH2_AGENT*& agent,
                     ITransportTarget& target);

    SshAuthenticator(const SshAuthenticator&)            = delete;
    SshAuthenticator& operator=(const SshAuthenticator&) = delete;

    // Worker-thread-only (uses the non-blocking session).
    Result Authenticate();

    // Reached by the file-local keyboard-interactive C callback through the
    // session abstract pointer.
    ITransportTarget& Target() { return target_; }

private:
    Result AuthViaAgent();
    Result AuthViaPassword();
    Result AuthViaPrivateKey();
    Result AuthViaKbdInteractive();

    std::vector<std::vector<uint8_t>> PreferredAgentKeyBlobs() const;
    // Tries agent identities whose blob matches one in `preferred`. Returns true
    // on the first accepted identity; sets *anyMatched if at least one matched.
    bool   AgentTryPreferred(_LIBSSH2_AGENT* agent,
                             const std::vector<std::vector<uint8_t>>& preferred,
                             bool* anyMatched);
    Result AgentTryAll(_LIBSSH2_AGENT* agent);

    bool PollUntilReady();

    const SshDesc&           desc_;
    _LIBSSH2_SESSION*&       session_;
    int&                     sockFd_;
    const std::atomic<bool>& running_;
    _LIBSSH2_AGENT*&         agent_;
    ITransportTarget&        target_;
};

} // namespace term::transport
