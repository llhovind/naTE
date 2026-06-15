#include "transport/SshSftpService.h"
#include "transport/SshSession.h"   // ssh::LastSshError

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <ctime>
#include <fstream>
#include <memory>

namespace term::transport {

namespace {

// Upper bound on how long EnsureSftp will wait for the SFTP subsystem to come
// up. Generous enough for a slow link's handshake, short enough that a remote
// without a working sftp-server fails with a clear message rather than hanging.
constexpr auto kSftpInitTimeout = std::chrono::seconds(15);

// Formats a POSIX permission bitmask as a "drwxr-xr-x" style string.
std::string SftpFormatPermissions(unsigned long mode)
{
    char buf[11];
    buf[0] = LIBSSH2_SFTP_S_ISDIR(mode) ? 'd' :
             LIBSSH2_SFTP_S_ISLNK(mode) ? 'l' : '-';
    const unsigned long bits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    const char         chars[3] = {'r', 'w', 'x'};
    for (int i = 0; i < 9; ++i)
        buf[1 + i] = (mode & bits[i]) ? chars[i % 3] : '-';
    buf[10] = '\0';
    return buf;
}

// Formats a Unix timestamp as "YYYY-MM-DD HH:MM".
std::string SftpFormatModTime(unsigned long mtime)
{
    char buf[32];
    const time_t t = static_cast<time_t>(mtime);
    struct tm tm{};
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// SFTP task state machines (nested types — have access to SftpService privates)
// ---------------------------------------------------------------------------

struct SftpService::ListDirTask {
    enum class State { InitSftp, OpenDir, ReadLoop };

    SftpService*  svc   = nullptr;
    State         state = State::InitSftp;
    std::string   path;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    std::vector<RemoteDirEntry> entries;
    std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone;

    ListDirTask() = default;
    ListDirTask(ListDirTask&&) = default;
    ListDirTask& operator=(ListDirTask&&) = default;
    ListDirTask(const ListDirTask&) = delete;
    ListDirTask& operator=(const ListDirTask&) = delete;

    ~ListDirTask() { if (handle) libssh2_sftp_closedir(handle); }

    bool operator()()
    {
        if (!svc->running_) {
            if (handle) { libssh2_sftp_closedir(handle); handle = nullptr; }
            onDone({}, "Session closed");
            return false;
        }

        if (state == State::InitSftp) {
            std::string err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  onDone({}, std::move(err)); return false;
                case InitResult::Ready:   break;
            }
            state = State::OpenDir;
        }

        if (state == State::OpenDir) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_opendir(svc->sftp_, path.c_str());
            if (!h) {
                if (libssh2_session_last_errno(svc->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                onDone({}, "Cannot list '" + path + "': " + ssh::LastSshError(svc->session_));
                return false;
            }
            handle = h;
            state  = State::ReadLoop;
        }

        // ReadLoop: drain all available entries this iteration.
        char namebuf[512];
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        while (true) {
            const int rc = libssh2_sftp_readdir_ex(
                handle, namebuf, sizeof(namebuf) - 1, nullptr, 0, &attrs);
            if (rc == LIBSSH2_ERROR_EAGAIN) return true;
            if (rc == 0) {
                libssh2_sftp_closedir(handle); handle = nullptr;
                auto cb   = std::move(onDone);
                auto ents = std::move(entries);
                cb(std::move(ents), {});
                return false;
            }
            if (rc < 0) {
                libssh2_sftp_closedir(handle); handle = nullptr;
                onDone({}, "Directory read error: " + ssh::LastSshError(svc->session_));
                return false;
            }
            namebuf[rc] = '\0';
            std::string name(namebuf, static_cast<size_t>(rc));
            if (name == "." || name == "..") continue;

            RemoteDirEntry e;
            e.name = std::move(name);
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
                e.size = attrs.filesize;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
                e.isDir       = LIBSSH2_SFTP_S_ISDIR(attrs.permissions) != 0;
                e.isSymlink   = LIBSSH2_SFTP_S_ISLNK(attrs.permissions) != 0;
                e.permissions = SftpFormatPermissions(attrs.permissions);
            }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                e.modTime = SftpFormatModTime(attrs.mtime);
            entries.push_back(std::move(e));
        }
    }
};

struct SftpService::DownloadTask {
    enum class State { InitSftp, OpenHandle, ReadLoop };

    SftpService*  svc   = nullptr;
    State         state = State::InitSftp;
    std::string   remotePath;
    std::string   localPath;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    std::ofstream out;
    std::function<void(bool, std::string)> onDone;

    DownloadTask() = default;
    DownloadTask(DownloadTask&&) = default;
    DownloadTask& operator=(DownloadTask&&) = default;
    DownloadTask(const DownloadTask&) = delete;
    DownloadTask& operator=(const DownloadTask&) = delete;

    ~DownloadTask() { if (handle) libssh2_sftp_close(handle); }

    bool operator()()
    {
        if (!svc->running_) {
            if (handle) { libssh2_sftp_close(handle); handle = nullptr; }
            onDone(false, "Session closed");
            return false;
        }

        if (state == State::InitSftp) {
            std::string err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  onDone(false, std::move(err)); return false;
                case InitResult::Ready:   break;
            }
            out.open(localPath, std::ios::binary | std::ios::trunc);
            if (!out) {
                onDone(false, "Cannot create local file: " + localPath);
                return false;
            }
            state = State::OpenHandle;
        }

        if (state == State::OpenHandle) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                svc->sftp_, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
            if (!h) {
                if (libssh2_session_last_errno(svc->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                onDone(false, "Cannot open '" + remotePath + "': " +
                              ssh::LastSshError(svc->session_));
                return false;
            }
            handle = h;
            state  = State::ReadLoop;
        }

        // ReadLoop: drain all available data this iteration.
        char buf[32768];
        while (true) {
            const ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) return true;
            if (n < 0) {
                libssh2_sftp_close(handle); handle = nullptr;
                onDone(false, "Read error: " + ssh::LastSshError(svc->session_));
                return false;
            }
            if (n == 0) {
                libssh2_sftp_close(handle); handle = nullptr;
                out.close();
                onDone(true, localPath);
                return false;
            }
            out.write(buf, n);
        }
    }
};

struct SftpService::UploadTask {
    enum class State { InitSftp, OpenHandle, WriteLoop };

    SftpService*  svc   = nullptr;
    State         state = State::InitSftp;
    std::string   localPath;
    std::string   remotePath;
    LIBSSH2_SFTP_HANDLE* handle  = nullptr;
    std::ifstream in;
    char          buf[32768]{};
    size_t        bufLen  = 0;
    size_t        bufSent = 0;
    std::function<void(bool, std::string)> onDone;

    UploadTask() = default;
    UploadTask(UploadTask&&) = default;
    UploadTask& operator=(UploadTask&&) = default;
    UploadTask(const UploadTask&) = delete;
    UploadTask& operator=(const UploadTask&) = delete;

    ~UploadTask() { if (handle) libssh2_sftp_close(handle); }

    bool operator()()
    {
        if (!svc->running_) {
            if (handle) { libssh2_sftp_close(handle); handle = nullptr; }
            onDone(false, "Session closed");
            return false;
        }

        if (state == State::InitSftp) {
            std::string err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  onDone(false, std::move(err)); return false;
                case InitResult::Ready:   break;
            }
            in.open(localPath, std::ios::binary);
            if (!in) {
                onDone(false, "Cannot open local file: " + localPath);
                return false;
            }
            state = State::OpenHandle;
        }

        if (state == State::OpenHandle) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                svc->sftp_, remotePath.c_str(),
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);  // 0644
            if (!h) {
                if (libssh2_session_last_errno(svc->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                onDone(false, "Cannot open remote '" + remotePath + "' for write: " +
                              ssh::LastSshError(svc->session_));
                return false;
            }
            handle = h;
            state  = State::WriteLoop;
        }

        // WriteLoop: send buffered data; read next chunk when buffer exhausted.
        while (true) {
            if (bufSent >= bufLen) {
                in.read(buf, sizeof(buf));
                bufLen  = static_cast<size_t>(in.gcount());
                bufSent = 0;
                if (bufLen == 0) {
                    libssh2_sftp_close(handle); handle = nullptr;
                    onDone(true, {});
                    return false;
                }
            }
            const ssize_t n = libssh2_sftp_write(handle, buf + bufSent, bufLen - bufSent);
            if (n == LIBSSH2_ERROR_EAGAIN) return true;
            if (n < 0) {
                libssh2_sftp_close(handle); handle = nullptr;
                onDone(false, "Write error: " + ssh::LastSshError(svc->session_));
                return false;
            }
            bufSent += static_cast<size_t>(n);
        }
    }
};

// ---------------------------------------------------------------------------
// SftpService
// ---------------------------------------------------------------------------

SftpService::SftpService(_LIBSSH2_SESSION*& session, const std::atomic<bool>& running)
    : session_(session), running_(running)
{}

SftpService::InitResult SftpService::EnsureSftp(std::string& err)
{
    if (sftp_) return InitResult::Ready;

    if (LIBSSH2_SFTP* s = libssh2_sftp_init(session_)) {
        sftp_ = s;
        initDeadline_.reset();
        return InitResult::Ready;
    }

    if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
        initDeadline_.reset();
        err = "SFTP unavailable: " + ssh::LastSshError(session_);
        return InitResult::Failed;
    }

    // EAGAIN: arm the deadline on first sight, then fail once it elapses.
    const auto now = std::chrono::steady_clock::now();
    if (!initDeadline_) initDeadline_ = now + kSftpInitTimeout;
    if (now >= *initDeadline_) {
        initDeadline_.reset();
        err = "SFTP unavailable: timed out starting the SFTP subsystem "
              "(is sftp-server installed on the remote?)";
        return InitResult::Failed;
    }
    return InitResult::Pending;
}

void SftpService::Service()
{
    Task task;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (queue_.empty()) return;
        task = std::move(queue_.front());
        queue_.pop_front();
    }
    const bool again = task();
    if (again) {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.push_front(std::move(task));
    }
}

void SftpService::CancelPending()
{
    std::deque<Task> pending;
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        pending.swap(queue_);
    }
    for (auto& task : pending)
        task();  // task checks !running_, calls onDone(false,...), returns false
}

void SftpService::Shutdown()
{
    if (sftp_) {
        libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Enqueue API
// ---------------------------------------------------------------------------

void SftpService::SendFile(const std::string& localPath,
                           const std::string& remoteDir,
                           std::function<void(bool, std::string)> onDone)
{
    std::string filename = localPath;
    if (const auto pos = filename.rfind('/'); pos != std::string::npos)
        filename = filename.substr(pos + 1);
    std::string remotePath = remoteDir;
    if (!remotePath.empty() && remotePath.back() != '/') remotePath += '/';
    remotePath += filename;
    UploadFromPath(localPath, remotePath, std::move(onDone));
}

void SftpService::ReceiveFile(const std::string& remotePath,
                              const std::string& localDir,
                              std::function<void(bool, std::string)> onDone)
{
    std::string filename = remotePath;
    if (const auto pos = filename.rfind('/'); pos != std::string::npos)
        filename = filename.substr(pos + 1);
    std::string localPath = localDir;
    if (!localPath.empty() && localPath.back() != '/') localPath += '/';
    localPath += filename;
    DownloadToPath(remotePath, localPath, std::move(onDone));
}

void SftpService::ListRemoteDirectory(
    const std::string& remotePath,
    std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone)
{
    auto t = std::make_shared<ListDirTask>();
    t->svc    = this;
    t->path   = remotePath;
    t->onDone = std::move(onDone);
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.emplace_back([t]() mutable { return (*t)(); });
}

void SftpService::DownloadToPath(const std::string& remotePath,
                                 const std::string& localPath,
                                 std::function<void(bool, std::string)> onDone)
{
    auto t = std::make_shared<DownloadTask>();
    t->svc        = this;
    t->remotePath = remotePath;
    t->localPath  = localPath;
    t->onDone     = std::move(onDone);
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.emplace_back([t]() mutable { return (*t)(); });
}

void SftpService::UploadFromPath(const std::string& localPath,
                                 const std::string& remotePath,
                                 std::function<void(bool, std::string)> onDone)
{
    auto t = std::make_shared<UploadTask>();
    t->svc        = this;
    t->localPath  = localPath;
    t->remotePath = remotePath;
    t->onDone     = std::move(onDone);
    std::lock_guard<std::mutex> lk(queueMutex_);
    queue_.emplace_back([t]() mutable { return (*t)(); });
}

} // namespace term::transport
