#pragma once
#include "transport/PortForward.h"
#include "transport/SshChannelProxy.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <poll.h>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_LISTENER;

namespace term::transport {

class ITransportTarget;

// Owns all local (-L) and remote (-R) port forwards for one SSH session:
// the listening sockets, the per-connection ChannelProxy pumps, the pending
// add/remove queue, and the status snapshot pushed to the UI.
//
// Threading: AddForward/RemoveForward are UI-thread (queue only, under mutex_).
// Everything else is worker-thread-only and must be called from the session's
// read/write loop. session_ and sockFd_ are bound by reference because the
// transport creates them on the worker thread after this object is built.
class PortForwardEngine {
public:
    PortForwardEngine(_LIBSSH2_SESSION*& session, int& sockFd,
                      const std::atomic<bool>& running, ITransportTarget& target);

    PortForwardEngine(const PortForwardEngine&)            = delete;
    PortForwardEngine& operator=(const PortForwardEngine&) = delete;

    // --- UI thread -----------------------------------------------------------
    void AddForward(const PortForwardDesc& desc);
    void RemoveForward(PortForwardId id);

    // --- Worker thread -------------------------------------------------------
    // Appends one pollfd per listen socket and proxy connection, recording the
    // base index and per-fd tags internally so ServiceConns can dispatch
    // without the caller threading them back through.
    void AppendPollFds(std::vector<pollfd>& pfds);
    // Consumes the poll results recorded by the most recent AppendPollFds:
    // accepts new connections (both directions) and pumps data each way.
    void ServiceConns(const std::vector<pollfd>& pfds, char* buf, size_t bufLen);
    // Drains pending add/remove requests, creating/destroying listeners.
    void ServiceQueue();
    // Closes every listener and proxy connection (read/write loop exit).
    void Teardown();

private:
    struct PfwAdd    { PortForwardDesc desc; };
    struct PfwRemove { PortForwardId   id;   };
    using  PfwPending = std::variant<PfwAdd, PfwRemove>;

    // Tag paired with each appended pollfd so ServiceConns dispatches to the
    // right forward/connection without an implicit shared-order cursor.
    struct PfwPollEntry {
        enum class Kind : uint8_t { LocalListen, LocalConn, RemoteConn };
        Kind   kind;
        size_t fwdIdx;
        size_t connIdx;  // unused for LocalListen
    };

    struct ActiveLocalFwd {
        PortForwardDesc           desc;
        int                       listen_fd = -1;
        std::vector<ChannelProxy> conns;
    };

    struct ActiveRemoteFwd {
        PortForwardDesc           desc;
        _LIBSSH2_LISTENER*        listener   = nullptr;
        int                       bound_port = 0;
        std::vector<ChannelProxy> conns;
    };

    bool PollUntilReady();
    // Collects current status (including persisted failures) and fires
    // OnPortForwardStatusChanged if the snapshot changed.
    void NotifyStatus();
    // Records a permanent failure for id and fires a status update. The failure
    // persists until the forward is explicitly removed via RemoveForward.
    void FireFailed(PortForwardId id, const std::string& error);
    static std::string ErrnoString(int err);

    _LIBSSH2_SESSION*&       session_;
    int&                     sockFd_;
    const std::atomic<bool>& running_;
    ITransportTarget&        target_;

    std::vector<ActiveLocalFwd>  localFwds_;
    std::vector<ActiveRemoteFwd> remoteFwds_;

    std::mutex                   mutex_;        // guards pending_ (UI vs worker)
    std::vector<PfwPending>      pending_;

    std::vector<PortForwardStatus>                 lastStatus_;
    std::unordered_map<PortForwardId, std::string> failed_;

    // Poll bookkeeping for the current loop iteration (set by AppendPollFds,
    // consumed by ServiceConns).
    size_t                    pollBase_ = 0;
    std::vector<PfwPollEntry> pollTags_;
};

} // namespace term::transport
