#pragma once
#include "fs/Dispatcher.h"
#include "fs/LinkTarget.h"
#include "transport/IRemoteFileSystem.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace term::fs {

// How many link lookups may be outstanding at the transport at once.
//
// One, and it is not a tuning choice. libssh2 keeps the state of an SFTP
// operation on the *session* — a single `stat_state`, `stat_packet` and
// `stat_request_id` for the whole connection, not one per call. A second stat
// issued while the first is awaiting its reply finds the state already "sent",
// skips building a packet of its own, and waits on the first request's id: it
// is handed the first stat's attributes and reports them as its own. Two links
// resolved at once therefore trade answers, and a link to an ISO comes back
// looking like the link to a directory beside it.
//
// The SFTP adapter now refuses to overlap them (see transport/SftpTaskQueue.h),
// because the port promises callers nothing about serialisation and two panes
// on one session can collide without any help from here. Issuing more than one
// anyway would only pile them up behind that gate, so one it is — and a
// navigation then waits on at most a single round trip, since cancelling stops
// everything not yet issued.
inline constexpr size_t kMaxConcurrentLinkLookups = 1;

// Resolves what a directory's symlinks point at.
//
// The one place that trades round trips for the two things a listing cannot
// tell on its own: whether a link leads to a directory, and whether it leads
// anywhere at all. Everything else about a link — that it is one, its mode,
// its size — comes free with the listing and is not this class's business.
//
// Results are delivered as one batch rather than row by row. Applying them
// re-sorts the listing, and a listing that reorders itself under the user's
// cursor several times in a row would be worse than one that reorders once,
// late.
//
// Wx-free and single-threaded: every transport callback is routed back through
// the dispatcher, so a caller only ever sees results on its own thread.
class LinkResolver {
public:
    // Fires exactly once per Resolve() call, with one entry per name asked
    // about — unless the batch is superseded or cancelled, in which case it
    // does not fire at all. A caller therefore never has to reconcile a stale
    // answer against a newer listing.
    using BatchCallback = std::function<void(std::vector<LinkResolution>)>;

    LinkResolver(transport::IRemoteFileSystem& remote, Dispatcher dispatch);
    ~LinkResolver();

    LinkResolver(const LinkResolver&)            = delete;
    LinkResolver& operator=(const LinkResolver&) = delete;

    // Looks up each name in `directory`. Any batch still running is abandoned
    // first: its answers describe a listing that is no longer on screen.
    void Resolve(std::string directory, std::vector<std::string> names,
                 BatchCallback onDone);

    // Abandons the running batch. Lookups already at the transport still
    // complete — there is no way to recall them — but their answers are
    // dropped and no further ones are issued.
    void Cancel();

private:
    struct Batch;

    // Advances the batch: issues what it can, delivers it once nothing is left
    // in flight and every name has been asked about.
    void Pump(const std::shared_ptr<Batch>& batch);
    // Fills the free concurrency slots. Split out so the re-entrancy guard has
    // exactly one thing to protect.
    void IssueLookups(const std::shared_ptr<Batch>& batch);

    transport::IRemoteFileSystem& remote_;
    DispatchGuard                 guard_;

    // True while IssueLookups is running; see Pump for why that matters.
    bool pumping_ = false;

    // Bumped by every Resolve and every Cancel. A completion whose generation
    // no longer matches belongs to a listing the user has moved on from.
    uint64_t generation_ = 0;
};

} // namespace term::fs
