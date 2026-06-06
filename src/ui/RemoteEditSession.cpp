#include "ui/RemoteEditSession.h"
#include <filesystem>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wx/app.h>
#include <cstring>

namespace ui {

namespace {
    // Encode one path component: replace '/' with '%2F'.
    std::string EncodePathComponent(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '/') out += "%2F";
            else          out += c;
        }
        return out;
    }
} // namespace

RemoteEditSession::RemoteEditSession(term::session::SessionId     sessionId,
                                     std::string                  remotePath,
                                     std::string                  localPath,
                                     term::session::SessionManager& sm)
    : sessionId_(sessionId)
    , remotePath_(std::move(remotePath))
    , localPath_(std::move(localPath))
    , sm_(sm)
    , alive_(std::make_shared<std::atomic<bool>>(true))
{}

RemoteEditSession::~RemoteEditSession()
{
    Stop();
}

std::string RemoteEditSession::MakeTempPath(const std::string& hostname,
                                             const std::string& remotePath)
{
    // Extract the filename from the remote path.
    const auto slash = remotePath.rfind('/');
    const std::string dir  = (slash != std::string::npos) ? remotePath.substr(0, slash) : "";
    const std::string file = (slash != std::string::npos) ? remotePath.substr(slash + 1) : remotePath;

    return "/tmp/nate-edit/" + hostname + "/" + EncodePathComponent(dir) + "/" + file;
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
    alive_->store(false, std::memory_order_release);

    if (stopPipe_[1] >= 0) {
        char b = 1;
        (void)write(stopPipe_[1], &b, 1);
    }

    if (watchThread_.joinable())
        watchThread_.join();

    if (inotifyFd_ >= 0) { close(inotifyFd_);   inotifyFd_ = -1; }
    if (stopPipe_[0] >= 0) { close(stopPipe_[0]); stopPipe_[0] = -1; }
    if (stopPipe_[1] >= 0) { close(stopPipe_[1]); stopPipe_[1] = -1; }

    std::error_code ec;
    const auto localFile = std::filesystem::path(localPath_);
    std::filesystem::remove(localFile, ec);
    // Remove the parent directory only if it is now empty.
    const auto parentDir = localFile.parent_path();
    std::filesystem::remove(parentDir, ec); // no-op if non-empty
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
        wxTheApp->CallAfter([this] {
            if (!alive_->load(std::memory_order_acquire))
                return;
            TriggerUpload();
        });
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

    std::weak_ptr<std::atomic<bool>> weakAlive = alive_;
    const std::string local  = localPath_;
    const std::string remote = remotePath_;

    sm_.SftpUploadFile(sessionId_, local, remote,
        [this, weakAlive](bool /*ok*/, std::string /*err*/) {
            auto strongAlive = weakAlive.lock();
            if (!strongAlive || !strongAlive->load(std::memory_order_acquire))
                return;

            wxTheApp->CallAfter([this, weakAlive] {
                auto sa = weakAlive.lock();
                if (!sa || !sa->load(std::memory_order_acquire))
                    return;

                uploadInFlight_.store(false, std::memory_order_relaxed);

                if (pendingUpload_.exchange(false))
                    TriggerUpload();
            });
        });
}

} // namespace ui
