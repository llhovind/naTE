#include "transport/PtyTransport.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>

#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif

namespace term::transport
{

PtyTransport::PtyTransport(std::string shell, unsigned short cols, unsigned short rows)
    : shell_(std::move(shell))
{
    struct winsize ws{};
    ws.ws_col = cols;
    ws.ws_row = rows;

    child_pid_ = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (child_pid_ < 0)
        throw std::runtime_error(std::string("forkpty failed: ") + std::strerror(errno));

    if (child_pid_ == 0) {
        // Child: exec the shell; _exit on failure so destructors don't run
        execl(shell_.c_str(), shell_.c_str(), nullptr);
        _exit(1);
    }

    // Parent
    running_ = true;
    reader_ = std::thread(&PtyTransport::ReadLoop, this);
}

PtyTransport::~PtyTransport()
{
    running_ = false;
    if (child_pid_ > 0)
        kill(child_pid_, SIGHUP);
    if (master_fd_ >= 0) {
        close(master_fd_);
        master_fd_ = -1;
    }
    if (reader_.joinable())
        reader_.join();
    if (child_pid_ > 0)
        waitpid(child_pid_, nullptr, WNOHANG);
}

void PtyTransport::Write(const std::string& data)
{
    if (master_fd_ < 0 || data.empty())
        return;
    ::write(master_fd_, data.data(), data.size());
}

void PtyTransport::SetReadCallback(DataCallback cb)
{
    callback_ = std::move(cb);
}

void PtyTransport::SetDisconnectCallback(DisconnectCallback cb)
{
    disconnect_callback_ = std::move(cb);
}

void PtyTransport::ReadLoop()
{
    constexpr int kBufSize = 4096;
    char buf[kBufSize];

    while (running_) {
        struct pollfd pfd{};
        pfd.fd     = master_fd_;
        pfd.events = POLLIN;

        const int ret = poll(&pfd, 1, 100 /*ms timeout*/);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break; // unrecoverable
        }
        if (ret == 0)
            continue; // timeout — loop to check running_

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        if (pfd.revents & POLLIN) {
            const ssize_t n = ::read(master_fd_, buf, kBufSize);
            if (n <= 0)
                break; // EIO when shell exits, or real error
            if (callback_)
                callback_(std::string(buf, static_cast<size_t>(n)));
        }
    }

    running_ = false;
    if (disconnect_callback_)
        disconnect_callback_();
}

} // namespace term::transport
