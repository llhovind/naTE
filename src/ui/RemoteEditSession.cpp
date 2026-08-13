#include "ui/RemoteEditSession.h"
#include "fs/EditTempPath.h"
#include <filesystem>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wx/app.h>
#include <cstring>

namespace ui {

RemoteEditSession::RemoteEditSession(term::session::SessionId     sessionId,
                                     std::string                  remotePath,
                                     std::string                  localPath,
                                     term::session::SessionManager& sm)
    : sessionId_(sessionId)
    , remotePath_(std::move(remotePath))
    , localPath_(std::move(localPath))
    , sm_(sm)
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
    term::fs::RemoveWorkingCopy(localPath_);
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

        // Marshal to UI thread for upload (all wx and manager calls must be UI-thread).
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
    sm_.UploadFile(sessionId_, local, remote,
        [ctx](term::transport::FsError err) mutable {
            ctx.Post([err = std::move(err)](RemoteEditSession& s) {
                s.OnUploadFinished(err);
            });
        });
}

void RemoteEditSession::OnUploadFinished(const term::transport::FsError& err)
{
    uploadInFlight_.store(false, std::memory_order_relaxed);

    // Only the last upload of a coalesced burst is worth announcing: a pending
    // upload is about to supersede this outcome either way, and a failure that
    // the very next attempt repairs was never the user's problem.
    const bool more = pendingUpload_.exchange(false);

    if (!more) {
        if (err.Failed()) {
            // Same failure twice running is the same broken state, not news.
            if (err.message != lastReportedError_) {
                lastReportedError_ = err.message;
                if (onSaveFailed_)
                    onSaveFailed_({sessionId_, remotePath_, localPath_, err.message});
            }
        } else {
            lastReportedError_.clear();
            if (onSaved_)
                onSaved_(sessionId_, remotePath_);
        }
    }

    if (more)
        TriggerUpload();
}

} // namespace ui
