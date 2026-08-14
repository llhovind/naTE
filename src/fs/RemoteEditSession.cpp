#include "fs/RemoteEditSession.h"
#include "fs/EditWorkspace.h"
#include <filesystem>
#include <optional>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <cstring>

namespace term::fs {

RemoteEditSession::RemoteEditSession(EditEndpoint endpoint,
                                     std::string  remotePath,
                                     std::string  localPath,
                                     Dispatcher   dispatch)
    : endpoint_(std::move(endpoint))
    , remotePath_(std::move(remotePath))
    , localPath_(std::move(localPath))
    , guard_(std::move(dispatch))
{}

RemoteEditSession::~RemoteEditSession()
{
    Stop();
}

void RemoteEditSession::Start()
{
    if (pipe(stopPipe_) != 0)
        return;

    const std::string watchDir = std::filesystem::path(localPath_).parent_path().string();

    inotifyFd_ = inotify_init1(IN_NONBLOCK);
    if (inotifyFd_ < 0)
        return;

    // Watch the parent directory; filter events to our specific filename in WatchLoop.
    inotify_add_watch(inotifyFd_, watchDir.c_str(),
                      IN_CLOSE_WRITE | IN_MOVED_TO);

    watchThread_ = std::thread([this]{ WatchLoop(); });
}

void RemoteEditSession::Stop()
{
    // First: everything below can block, and no callback may reach this session
    // while it is being taken apart.
    guard_.Retire();

    if (stopPipe_[1] >= 0) {
        char b = 1;
        (void)write(stopPipe_[1], &b, 1);
    }

    if (watchThread_.joinable())
        watchThread_.join();

    if (inotifyFd_ >= 0) { close(inotifyFd_);   inotifyFd_ = -1; }
    if (stopPipe_[0] >= 0) { close(stopPipe_[0]); stopPipe_[0] = -1; }
    if (stopPipe_[1] >= 0) { close(stopPipe_[1]); stopPipe_[1] = -1; }

    // The working copy sits in a directory of its own, so this takes that with
    // it, and any level above it the removal leaves empty.
    RemoveWorkingCopy(localPath_);
}

void RemoteEditSession::WatchLoop()
{
    const std::string targetName = std::filesystem::path(localPath_).filename().string();

    struct pollfd pfds[2];
    pfds[0].fd     = inotifyFd_;
    pfds[0].events = POLLIN;
    pfds[1].fd     = stopPipe_[0];
    pfds[1].events = POLLIN;

    alignas(struct inotify_event) char buf[4096];

    while (true) {
        const int ret = poll(pfds, 2, -1);
        if (ret <= 0)
            break;

        if (pfds[1].revents & POLLIN)
            break; // stop requested

        if (!(pfds[0].revents & POLLIN))
            continue;

        const ssize_t n = read(inotifyFd_, buf, sizeof(buf));
        if (n <= 0)
            continue;

        bool trigger = false;
        const char* p = buf;
        while (p < buf + n) {
            const auto* ev = reinterpret_cast<const struct inotify_event*>(p);
            if ((ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) && ev->len > 0) {
                if (std::string(ev->name) == targetName)
                    trigger = true;
            }
            p += sizeof(struct inotify_event) + ev->len;
        }

        if (!trigger)
            continue;

        // Marshal to the owning thread for upload: everything downstream of it
        // is single-threaded by construction.
        guard_.For(this).Post([](RemoteEditSession& s) { s.TriggerUpload(); });
    }
}

void RemoteEditSession::TriggerUpload()
{
    // If an upload is already in flight, record the pending request and return.
    // The completion callback will re-trigger.
    if (uploadInFlight_.exchange(true))  {
        pendingUpload_.store(true, std::memory_order_relaxed);
        return;
    }

    const std::string local  = localPath_;
    const std::string remote = remotePath_;

    // One liveness check, on the thread that acts on it. The old outer check on
    // the transport's thread could only ever be a stale hint.
    auto ctx = guard_.For(this);

    // No mode: this writes back over a file that already exists, whose
    // permissions are the user's and must survive the save untouched.
    endpoint_.fs->Upload(local, remote, std::nullopt, nullptr,
        [ctx](transport::FsError err) mutable {
            ctx.Post([err = std::move(err)](RemoteEditSession& s) {
                s.OnUploadFinished(err);
            });
        });
}

void RemoteEditSession::OnUploadFinished(const transport::FsError& err)
{
    uploadInFlight_.store(false, std::memory_order_relaxed);

    // Taken before the policy is consulted, and consumed either way: this
    // upload's outcome is only the burst's last word if nothing is queued
    // behind it.
    const bool more = pendingUpload_.exchange(false);

    switch (announce_.Decide(err, more)) {
        case SaveAnnouncement::Saved:
            if (onSaved_) onSaved_(endpoint_.fs, remotePath_);
            break;
        case SaveAnnouncement::Failed:
            if (onSaveFailed_)
                onSaveFailed_({endpoint_.fs, remotePath_, localPath_, err.message});
            break;
        case SaveAnnouncement::Nothing:
            break;
    }

    if (more)
        TriggerUpload();
}

} // namespace term::fs
