#include "transport/PtyTransport.h"
#include "transport/EnvUtils.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#ifdef __APPLE__
#  include <util.h>
#else
#  include <pty.h>
#endif

namespace term::transport {

namespace {

std::string GenerateVpColumnsFilePath() {
    static std::atomic<int> counter{0};
    return "/tmp/nate-vpcolumns-"
         + std::to_string(::getpid())
         + "-"
         + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

} // namespace

PtyTransport::PtyTransport(ITransportTarget& target,
                           std::string shell,
                           unsigned short cols,
                           unsigned short rows,
                           unsigned short viewportCols,
                           const term::session::SessionInit& sessionInit,
                           const term::session::AppSessionDefaults& appDefaults)
    : target_(target), shell_(std::move(shell)),
      vpcolumns_file_(GenerateVpColumnsFilePath())
{
    struct winsize ws{};
    ws.ws_col = cols;
    ws.ws_row = rows;

    child_pid_ = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (child_pid_ < 0)
        throw std::runtime_error(std::string("forkpty failed: ") + std::strerror(errno));

    if (child_pid_ == 0) {
        // Child process: apply session initialisation before exec.
        const char* homeRaw = getenv("HOME");
        const std::string homeDir = homeRaw ? homeRaw : "";

        // Determine which env file to use: profile wins over app default.
        const std::string& rawEnvFile = sessionInit.envFilePath.empty()
            ? appDefaults.envFilePath
            : sessionInit.envFilePath;
        const std::string envFile = ExpandTilde(rawEnvFile, homeDir);

        const std::vector<std::string> parentEnv = CaptureParentEnv();
        const std::vector<term::session::EnvVar> fileVars = ParseEnvFile(envFile);

        // Inject NATE_VPCOLUMNS_FILE alongside the profile vars so the shell
        // can read the actual viewport width even when COLUMNS is inflated.
        auto appVars = appDefaults.envVars;
        appVars.push_back({"NATE_VPCOLUMNS_FILE", vpcolumns_file_});

        EnvBlock envBlock = BuildEnvBlock(parentEnv, appVars, fileVars, sessionInit.envVars);

        const std::string resolvedDir = ResolveWorkingDir(
            sessionInit.workingDir, appDefaults.workingDir, homeDir);
        if (!resolvedDir.empty())
            chdir(resolvedDir.c_str()); // silent on failure — shell will report its own cwd

        const bool useLogin = sessionInit.loginShell || appDefaults.loginShell;
        const std::string argv0 = useLogin ? MakeLoginShellArg0(shell_) : shell_;

        const char* args[] = { argv0.c_str(), nullptr };
        execve(shell_.c_str(), const_cast<char* const*>(args), envBlock.ptrs.data());
        _exit(1);
    }
    // Parent: write the initial viewport width so the file exists before the shell
    // runs its first command.
    WriteVpColumns(vpcolumns_file_, viewportCols);
}

PtyTransport::~PtyTransport()
{
    Stop();
    ::unlink(vpcolumns_file_.c_str());
}

void PtyTransport::Stop()
{
    running_ = false;
    const pid_t pid = std::exchange(child_pid_, -1);
    if (pid > 0)
        kill(pid, SIGHUP);
    if (master_fd_ >= 0) {
        close(master_fd_);
        master_fd_ = -1;
    }
    if (reader_.joinable())
        reader_.join();
    if (pid > 0)
        waitpid(pid, nullptr, WNOHANG);
}

void PtyTransport::Write(const std::string& data)
{
    if (master_fd_ < 0 || data.empty())
        return;
    ::write(master_fd_, data.data(), data.size());
}

void PtyTransport::Start()
{
    running_ = true;
    reader_ = std::thread(&PtyTransport::ReadLoop, this);
}

void PtyTransport::Resize(unsigned short cols, unsigned short rows)
{
    if (master_fd_ < 0)
        return;
    struct winsize ws{};
    ws.ws_col = cols;
    ws.ws_row = rows;
    ioctl(master_fd_, TIOCSWINSZ, &ws);
}

void PtyTransport::OnViewportColsChanged(unsigned short cols)
{
    WriteVpColumns(vpcolumns_file_, cols);
}

// static
void PtyTransport::WriteVpColumns(const std::string& path, unsigned short cols)
{
    if (path.empty()) return;
    std::ofstream f(path, std::ios::trunc);
    if (f) f << cols << '\n';
}

void PtyTransport::ReadLoop()
{
    constexpr int kBufSize = 4096;
    char buf[kBufSize];

    while (running_) {
        struct pollfd pfd{};
        pfd.fd     = master_fd_;
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
            const ssize_t n = ::read(master_fd_, buf, kBufSize);
            if (n <= 0)
                break;
            target_.OnData(std::string(buf, static_cast<size_t>(n)));
        }
    }

    running_ = false;
    target_.OnDisconnect();
}

} // namespace term::transport
