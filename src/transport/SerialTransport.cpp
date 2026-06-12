#include "transport/SerialTransport.h"
#include "transport/EnvUtils.h"
#include "transport/TransportError.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace term::transport {

namespace {

speed_t BaudToSpeed(unsigned int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default:     return B115200;
    }
}

} // namespace

SerialTransport::SerialTransport(ITransportTarget& target,
                                 const term::transport::SerialDesc& desc,
                                 const term::transport::SessionInit& sessionInit,
                                 const term::transport::AppSessionDefaults& appDefaults)
    : target_(target), desc_(desc), sessionInit_(sessionInit), appDefaults_(appDefaults)
{
    fd_ = ::open(desc_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0)
        throw std::runtime_error(std::string("serial open failed (") +
                                 desc_.device + "): " + std::strerror(errno));
    ConfigureTty();
}

SerialTransport::~SerialTransport()
{
    Stop();
}

void SerialTransport::ConfigureTty()
{
    struct termios tty{};
    if (tcgetattr(fd_, &tty) != 0)
        throw std::runtime_error(std::string("tcgetattr: ") + std::strerror(errno));

    cfmakeraw(&tty);

    const speed_t speed = BaudToSpeed(desc_.baudRate);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // Data bits
    tty.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
    switch (desc_.dataBits) {
        case 5:  tty.c_cflag |= CS5; break;
        case 6:  tty.c_cflag |= CS6; break;
        case 7:  tty.c_cflag |= CS7; break;
        default: tty.c_cflag |= CS8; break;
    }

    // Stop bits
    if (desc_.stopBits == term::transport::SerialStopBits::Two)
        tty.c_cflag |= CSTOPB;
    else
        tty.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);

    // Parity
    switch (desc_.parity) {
        case term::transport::SerialParity::Even:
            tty.c_cflag |=  PARENB;
            tty.c_cflag &= ~static_cast<tcflag_t>(PARODD);
            break;
        case term::transport::SerialParity::Odd:
            tty.c_cflag |= PARENB | PARODD;
            break;
        default:
            tty.c_cflag &= ~static_cast<tcflag_t>(PARENB);
            break;
    }

    // Flow control
    switch (desc_.flowControl) {
        case term::transport::SerialFlowControl::Hardware:
            tty.c_cflag |=  CRTSCTS;
            tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF);
            break;
        case term::transport::SerialFlowControl::Software:
            tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
            tty.c_iflag |=  IXON | IXOFF;
            break;
        default:
            tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
            tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF);
            break;
    }

    // Enable receiver, local mode (no modem control lines for open)
    tty.c_cflag |= CREAD | CLOCAL;

    // Non-blocking read: return immediately if no data
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0)
        throw std::runtime_error(std::string("tcsetattr: ") + std::strerror(errno));
}

void SerialTransport::RunDialScript()
{
    const pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error(std::string("fork for dial script: ") + std::strerror(errno));

    if (pid == 0) {
        // Child: wire the serial fd to stdin/stdout so the script (e.g. chat(8))
        // can read/write the port transparently.
        ::dup2(fd_, STDIN_FILENO);
        ::dup2(fd_, STDOUT_FILENO);

        // Apply merged env block so the dial script inherits session-init env vars.
        const char* homeRaw = getenv("HOME");
        const std::string homeDir = homeRaw ? homeRaw : "";
        const std::string& rawEnvFile = sessionInit_.envFilePath.empty()
            ? appDefaults_.envFilePath
            : sessionInit_.envFilePath;
        const std::vector<term::transport::EnvVar> fileVars =
            ParseEnvFile(ExpandTilde(rawEnvFile, homeDir));
        const std::vector<std::string> parentEnv = CaptureParentEnv();
        EnvBlock envBlock = BuildEnvBlock(
            parentEnv, appDefaults_.envVars, fileVars, sessionInit_.envVars);

        const char* args[] = { "sh", "-c", desc_.dialScript.c_str(), nullptr };
        ::execve("/bin/sh", const_cast<char* const*>(args), envBlock.ptrs.data());
        ::_exit(127);
    }

    // Parent: wait for the script to finish.
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        target_.OnError(TransportError{
            TransportError::Category::Connection,
            "Dial script failed (exit " +
                std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + "): " +
                desc_.dialScript
        });
        // Stop() will be called by the session; don't call OnDisconnect here.
        running_ = false;
    }
}

void SerialTransport::Start()
{
    running_ = true;

    if (!desc_.dialScript.empty()) {
        RunDialScript();
        if (!running_)
            return;
    }

    reader_ = std::thread(&SerialTransport::ReadLoop, this);
}

void SerialTransport::Stop()
{
    running_ = false;
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (reader_.joinable())
        reader_.join();
}

void SerialTransport::Write(const std::string& data)
{
    if (fd_ < 0 || data.empty())
        return;
    ::write(fd_, data.data(), data.size());
}

void SerialTransport::Resize(unsigned short /*cols*/, unsigned short /*rows*/)
{
    // Serial ports have no PTY window size — intentional no-op.
}

void SerialTransport::ReadLoop()
{
    constexpr int kBufSize = 4096;
    char buf[kBufSize];

    while (running_) {
        struct pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;

        const int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
            continue;

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            break;

        if (pfd.revents & POLLIN) {
            const ssize_t n = ::read(fd_, buf, kBufSize);
            if (n <= 0)
                break;
            target_.OnData(std::string(buf, static_cast<size_t>(n)));
        }
    }

    // If Stop() was called, running_ is already false — deliberate close.
    // Any other exit (POLLHUP, read error) means the device was removed.
    const DisconnectReason reason = running_.load(std::memory_order_relaxed)
        ? DisconnectReason::Interrupted
        : DisconnectReason::Deliberate;
    running_ = false;
    target_.OnDisconnect(reason);
}

} // namespace term::transport
