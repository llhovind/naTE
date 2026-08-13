#include "transport/SshSftpService.h"
#include "transport/SshSession.h"   // ssh::LastSshError

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <fstream>
#include <memory>

namespace term::transport {

namespace {

// Upper bound on how long EnsureSftp will wait for the SFTP subsystem to come
// up. Generous enough for a slow link's handshake, short enough that a remote
// without a working sftp-server fails with a clear message rather than hanging.
constexpr auto kSftpInitTimeout = std::chrono::seconds(15);

// Minimum gap between progress callbacks for a single transfer. A transfer
// yields to the poll loop many times a second on a fast link; without this
// every yield would wake the UI thread for a few kilobytes of movement.
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

// I/O chunk size for transfers, and the largest filename the server may return.
constexpr size_t kTransferChunk   = 32768;
constexpr size_t kMaxFilenameLen  = 512;
constexpr size_t kMaxLongEntryLen = 1024;
// Upper bound for a resolved path from realpath/readlink. PATH_MAX on Linux.
constexpr size_t kMaxPathLen      = 4096;

// Translates an SFTP status code into a typed category. Anything unrecognised
// stays Protocol so a caller never mistakes an unknown failure for a specific
// one it knows how to handle.
FsErrorCode ClassifySftpStatus(unsigned long status)
{
    switch (status) {
        case LIBSSH2_FX_NO_SUCH_FILE:
        case LIBSSH2_FX_NO_SUCH_PATH:        return FsErrorCode::NoSuchFile;
        case LIBSSH2_FX_PERMISSION_DENIED:
        case LIBSSH2_FX_WRITE_PROTECT:       return FsErrorCode::PermissionDenied;
        case LIBSSH2_FX_FILE_ALREADY_EXISTS: return FsErrorCode::AlreadyExists;
        case LIBSSH2_FX_DIR_NOT_EMPTY:       return FsErrorCode::DirectoryNotEmpty;
        case LIBSSH2_FX_NOT_A_DIRECTORY:     return FsErrorCode::NotADirectory;
        case LIBSSH2_FX_NO_CONNECTION:
        case LIBSSH2_FX_CONNECTION_LOST:     return FsErrorCode::NotConnected;
        // The server understood and declined — most often a symlink request to
        // an implementation that has none. A caller can offer an alternative
        // rather than showing a protocol error.
        case LIBSSH2_FX_OP_UNSUPPORTED:      return FsErrorCode::Unsupported;
        default:                             return FsErrorCode::Protocol;
    }
}

// Copies libssh2 attributes into a FileInfo, honouring the presence flags:
// a server that omits a field must leave the default in place rather than
// having a garbage value read out of the union.
void ApplyAttributes(const LIBSSH2_SFTP_ATTRIBUTES& attrs, FileInfo& info)
{
    if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
        info.size = attrs.filesize;
    if (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        info.uid = static_cast<uint32_t>(attrs.uid);
        info.gid = static_cast<uint32_t>(attrs.gid);
    }
    if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        info.mode      = static_cast<uint32_t>(attrs.permissions);
        info.isDir     = LIBSSH2_SFTP_S_ISDIR(attrs.permissions)  != 0;
        info.isSymlink = LIBSSH2_SFTP_S_ISLNK(attrs.permissions) != 0;
    }
    if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        info.mtime = static_cast<int64_t>(attrs.mtime);
}

// Extracts owner and group names from an `ls -l` style long entry:
//
//   -rw-r--r--  1 root  wheel  1234 Jan  1 00:00 name
//                 ^^^^  ^^^^^
//
// SFTP attributes carry numeric uid/gid only, so this is the sole source of
// names. Only fields 2 and 3 are read — both precede the filename, so an entry
// whose name contains spaces cannot corrupt them. A server that formats its
// long entry differently simply yields no names, which is why FileInfo treats
// them as optional rather than assuming they are always present.
// Deliberately a hand-rolled scan rather than an istringstream: this runs once
// per directory entry, and a directory with tens of thousands of files should
// not pay for a stream construction each time.
void ParseOwnerFromLongEntry(const char* longEntry, FileInfo& info)
{
    if (!longEntry || !*longEntry) return;

    // Eight fields is the minimum a well-formed `ls -l` line can have. Stop
    // there rather than scanning a potentially long filename.
    constexpr int kMinFields   = 8;
    constexpr int kOwnerField  = 2;
    constexpr int kGroupField  = 3;

    const char* p = longEntry;
    std::string owner, group;
    int found = 0;

    while (*p && found < kMinFields) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;

        if (found == kOwnerField)      owner.assign(start, static_cast<size_t>(p - start));
        else if (found == kGroupField) group.assign(start, static_cast<size_t>(p - start));
        ++found;
    }

    // Only publish names from a line that actually has the expected shape; a
    // server formatting its long entry differently must yield nothing rather
    // than two arbitrary tokens.
    if (found < kMinFields) return;
    info.owner = std::move(owner);
    info.group = std::move(group);
}

} // namespace

// ---------------------------------------------------------------------------
// SFTP task state machines (nested types — have access to SftpService privates)
// ---------------------------------------------------------------------------

// Shared shape for every operation that issues one libssh2 call returning an
// int status: mkdir, rmdir, unlink, rename, setstat. The call itself is the
// only thing that varies, so it is injected rather than duplicated five times.
struct SftpService::SimpleOpTask {
    SftpService* svc      = nullptr;
    bool         initDone = false;
    // Set by whichever operation built this task; every one of them drives a
    // different libssh2 state machine.
    Slot         opSlot   = Slot::Stat;
    std::string  context;
    // Attribute block for setstat. Lives here rather than in the operation
    // lambda because libssh2 reads it on every EAGAIN retry, so it must
    // outlive a single invocation.
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    std::function<int(SimpleOpTask&)> op;
    DoneCallback onDone;

    Slot CurrentSlot() const { return initDone ? opSlot : Slot::Init; }

    bool operator()()
    {
        if (svc->Stopped()) {
            onDone(FsError::Make(FsErrorCode::NotConnected, "Session closed"));
            return false;
        }

        if (!initDone) {
            FsError err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  onDone(std::move(err)); return false;
                case InitResult::Ready:   break;
            }
            initDone = true;
        }

        const int rc = op(*this);
        if (rc == LIBSSH2_ERROR_EAGAIN) return true;
        if (rc < 0) {
            onDone(svc->MakeError(context));
            return false;
        }
        onDone(FsError::Success());
        return false;
    }
};

// realpath and readlink: same libssh2 entry point, different flag, both
// returning a resolved path.
struct SftpService::PathOpTask {
    SftpService* svc      = nullptr;
    bool         initDone = false;
    std::string  path;
    std::string  context;
    int          flag = 0;
    PathCallback onDone;

    // realpath, readlink and symlink are one state machine in libssh2, so all
    // three queue behind each other.
    Slot CurrentSlot() const { return initDone ? Slot::Symlink : Slot::Init; }

    bool operator()()
    {
        if (svc->Stopped()) {
            onDone({}, FsError::Make(FsErrorCode::NotConnected, "Session closed"));
            return false;
        }

        if (!initDone) {
            FsError err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  onDone({}, std::move(err)); return false;
                case InitResult::Ready:   break;
            }
            initDone = true;
        }

        char buf[kMaxPathLen];
        const int rc = libssh2_sftp_symlink_ex(
            svc->sftp_, path.c_str(), static_cast<unsigned>(path.size()),
            buf, static_cast<unsigned>(sizeof(buf)), flag);
        if (rc == LIBSSH2_ERROR_EAGAIN) return true;
        if (rc < 0) {
            onDone({}, svc->MakeError(context));
            return false;
        }
        onDone(std::string(buf, static_cast<size_t>(rc)), FsError::Success());
        return false;
    }
};

struct SftpService::StatTask {
    SftpService* svc      = nullptr;
    bool         initDone = false;
    std::string  path;
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    StatCallback onDone;

    Slot CurrentSlot() const { return initDone ? Slot::Stat : Slot::Init; }

    bool operator()()
    {
        if (svc->Stopped()) {
            onDone({}, FsError::Make(FsErrorCode::NotConnected, "Session closed"));
            return false;
        }

        if (!initDone) {
            FsError err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  onDone({}, std::move(err)); return false;
                case InitResult::Ready:   break;
            }
            initDone = true;
        }

        const int rc = libssh2_sftp_stat_ex(
            svc->sftp_, path.c_str(), static_cast<unsigned>(path.size()),
            LIBSSH2_SFTP_STAT, &attrs);
        if (rc == LIBSSH2_ERROR_EAGAIN) return true;
        if (rc < 0) {
            onDone({}, svc->MakeError("Cannot stat '" + path + "'"));
            return false;
        }

        // name is deliberately left empty: the caller supplied the path and
        // already knows the leaf, and deriving it here would make transport/
        // depend on the path helpers that consume FileInfo.
        FileInfo info;
        ApplyAttributes(attrs, info);
        onDone(std::move(info), FsError::Success());
        return false;
    }
};

struct SftpService::ListDirTask {
    enum class State { InitSftp, OpenDir, ReadLoop };

    SftpService*  svc   = nullptr;
    State         state = State::InitSftp;
    std::string   path;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    std::vector<FileInfo> entries;
    ListCallback  onDone;

    ListDirTask() = default;
    ListDirTask(const ListDirTask&) = delete;
    ListDirTask& operator=(const ListDirTask&) = delete;

    ~ListDirTask() { if (handle) libssh2_sftp_closedir(handle); }

    Slot CurrentSlot() const
    {
        switch (state) {
            case State::InitSftp: return Slot::Init;
            case State::OpenDir:  return Slot::Open;
            case State::ReadLoop: return Slot::ReadDir;
        }
        return Slot::Open;
    }

    // Hands back whatever was read alongside the outcome. A directory that
    // fails partway is still useful, and discarding it would be a worse lie
    // than showing it with the error attached.
    bool Finish(FsError err)
    {
        if (handle) { libssh2_sftp_closedir(handle); handle = nullptr; }
        auto cb   = std::move(onDone);
        auto ents = std::move(entries);
        cb(std::move(ents), std::move(err));
        return false;
    }

    bool operator()()
    {
        if (svc->Stopped())
            return Finish(FsError::Make(FsErrorCode::NotConnected, "Session closed"));

        if (state == State::InitSftp) {
            FsError err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  return Finish(std::move(err));
                case InitResult::Ready:   break;
            }
            state = State::OpenDir;
        }

        if (state == State::OpenDir) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_opendir(svc->sftp_, path.c_str());
            if (!h) {
                if (libssh2_session_last_errno(svc->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                return Finish(svc->MakeError("Cannot list '" + path + "'"));
            }
            handle = h;
            state  = State::ReadLoop;
        }

        // ReadLoop: drain all available entries this iteration.
        char namebuf[kMaxFilenameLen];
        char longentry[kMaxLongEntryLen];
        LIBSSH2_SFTP_ATTRIBUTES attrs{};
        while (true) {
            longentry[0] = '\0';
            const int rc = libssh2_sftp_readdir_ex(
                handle, namebuf, sizeof(namebuf) - 1,
                longentry, sizeof(longentry) - 1, &attrs);
            if (rc == LIBSSH2_ERROR_EAGAIN) return true;
            if (rc == 0) return Finish(FsError::Success());
            if (rc < 0)  return Finish(svc->MakeError("Directory read error in '" + path + "'"));

            namebuf[rc] = '\0';
            longentry[sizeof(longentry) - 1] = '\0';

            std::string name(namebuf, static_cast<size_t>(rc));
            if (name == "." || name == "..") continue;

            FileInfo e;
            e.name = std::move(name);
            ApplyAttributes(attrs, e);
            ParseOwnerFromLongEntry(longentry, e);
            entries.push_back(std::move(e));
        }
    }
};

struct SftpService::DownloadTask {
    enum class State { InitSftp, OpenHandle, FStat, ReadLoop };

    SftpService*  svc   = nullptr;
    State         state = State::InitSftp;
    TransferHandle handleId = kInvalidTransferHandle;
    std::string   remotePath;
    std::string   localPath;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    std::ofstream out;
    uint64_t      transferred = 0;
    uint64_t      total       = 0;
    std::chrono::steady_clock::time_point lastProgress{};
    ProgressCallback onProgress;
    DoneCallback  onDone;

    DownloadTask() = default;
    DownloadTask(const DownloadTask&) = delete;
    DownloadTask& operator=(const DownloadTask&) = delete;

    ~DownloadTask() { if (handle) libssh2_sftp_close(handle); }

    Slot CurrentSlot() const
    {
        switch (state) {
            case State::InitSftp:    return Slot::Init;
            case State::OpenHandle:  return Slot::Open;
            case State::FStat:       return Slot::FStat;
            case State::ReadLoop:    return Slot::Read;
        }
        return Slot::Read;
    }

    bool Finish(FsError err)
    {
        if (handle) { libssh2_sftp_close(handle); handle = nullptr; }
        out.close();
        svc->ForgetCancellation(handleId);
        if (err.Ok() && onProgress) onProgress(transferred, total);
        onDone(std::move(err));
        return false;
    }

    // Emits progress no more often than kProgressInterval.
    void MaybeReportProgress()
    {
        if (!onProgress) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastProgress < kProgressInterval) return;
        lastProgress = now;
        onProgress(transferred, total);
    }

    bool operator()()
    {
        if (svc->Stopped())
            return Finish(FsError::Make(FsErrorCode::NotConnected, "Session closed"));
        if (svc->IsCancelled(handleId))
            return Finish(FsError::Make(FsErrorCode::Cancelled, "Transfer cancelled"));

        if (state == State::InitSftp) {
            FsError err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  return Finish(std::move(err));
                case InitResult::Ready:   break;
            }
            out.open(localPath, std::ios::binary | std::ios::trunc);
            if (!out)
                return Finish(FsError::Make(FsErrorCode::LocalIoError,
                                            "Cannot create local file: " + localPath));
            state = State::OpenHandle;
        }

        if (state == State::OpenHandle) {
            LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(
                svc->sftp_, remotePath.c_str(), LIBSSH2_FXF_READ, 0);
            if (!h) {
                if (libssh2_session_last_errno(svc->session_) == LIBSSH2_ERROR_EAGAIN)
                    return true;
                return Finish(svc->MakeError("Cannot open '" + remotePath + "'"));
            }
            handle = h;
            state  = State::FStat;
        }

        if (state == State::FStat) {
            // Best-effort: the size drives the progress denominator, so a
            // server that refuses fstat costs an indeterminate progress bar,
            // not a failed transfer.
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            const int rc = libssh2_sftp_fstat_ex(handle, &attrs, 0);
            if (rc == LIBSSH2_ERROR_EAGAIN) return true;
            if (rc == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
                total = attrs.filesize;
            state = State::ReadLoop;
        }

        // ReadLoop: drain all available data this iteration.
        char buf[kTransferChunk];
        while (true) {
            const ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) { MaybeReportProgress(); return true; }
            if (n < 0)
                return Finish(svc->MakeError("Read error on '" + remotePath + "'"));
            if (n == 0)
                return Finish(FsError::Success());

            out.write(buf, n);
            if (!out)
                return Finish(FsError::Make(FsErrorCode::LocalIoError,
                                            "Write failed on local file: " + localPath));
            transferred += static_cast<uint64_t>(n);
        }
    }
};

struct SftpService::UploadTask {
    enum class State { InitSftp, OpenHandle, WriteLoop };

    SftpService*  svc   = nullptr;
    State         state = State::InitSftp;
    TransferHandle handleId = kInvalidTransferHandle;
    std::string   localPath;
    std::string   remotePath;
    LIBSSH2_SFTP_HANDLE* handle  = nullptr;
    std::ifstream in;
    char          buf[kTransferChunk]{};
    size_t        bufLen  = 0;
    size_t        bufSent = 0;
    uint64_t      transferred = 0;
    uint64_t      total       = 0;
    std::chrono::steady_clock::time_point lastProgress{};
    ProgressCallback onProgress;
    DoneCallback  onDone;

    UploadTask() = default;
    UploadTask(const UploadTask&) = delete;
    UploadTask& operator=(const UploadTask&) = delete;

    ~UploadTask() { if (handle) libssh2_sftp_close(handle); }

    Slot CurrentSlot() const
    {
        switch (state) {
            case State::InitSftp:   return Slot::Init;
            case State::OpenHandle: return Slot::Open;
            case State::WriteLoop:  return Slot::Write;
        }
        return Slot::Write;
    }

    bool Finish(FsError err)
    {
        if (handle) { libssh2_sftp_close(handle); handle = nullptr; }
        in.close();
        svc->ForgetCancellation(handleId);
        if (err.Ok() && onProgress) onProgress(transferred, total);
        onDone(std::move(err));
        return false;
    }

    void MaybeReportProgress()
    {
        if (!onProgress) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastProgress < kProgressInterval) return;
        lastProgress = now;
        onProgress(transferred, total);
    }

    bool operator()()
    {
        if (svc->Stopped())
            return Finish(FsError::Make(FsErrorCode::NotConnected, "Session closed"));
        if (svc->IsCancelled(handleId))
            return Finish(FsError::Make(FsErrorCode::Cancelled, "Transfer cancelled"));

        if (state == State::InitSftp) {
            FsError err;
            switch (svc->EnsureSftp(err)) {
                case InitResult::Pending: return true;
                case InitResult::Failed:  return Finish(std::move(err));
                case InitResult::Ready:   break;
            }
            in.open(localPath, std::ios::binary | std::ios::ate);
            if (!in)
                return Finish(FsError::Make(FsErrorCode::LocalIoError,
                                            "Cannot open local file: " + localPath));
            total = static_cast<uint64_t>(in.tellg());
            in.seekg(0, std::ios::beg);
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
                return Finish(svc->MakeError("Cannot open remote '" + remotePath +
                                             "' for write"));
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
                    if (in.bad())
                        return Finish(FsError::Make(FsErrorCode::LocalIoError,
                                                    "Read failed on local file: " + localPath));
                    return Finish(FsError::Success());
                }
            }
            const ssize_t n = libssh2_sftp_write(handle, buf + bufSent, bufLen - bufSent);
            if (n == LIBSSH2_ERROR_EAGAIN) { MaybeReportProgress(); return true; }
            if (n < 0)
                return Finish(svc->MakeError("Write error on '" + remotePath + "'"));
            // A zero-length write is neither an error nor progress. Yielding
            // treats it as the EAGAIN it behaves like; looping on it would spin
            // this thread — which also drives the terminal session — forever.
            if (n == 0) { MaybeReportProgress(); return true; }
            bufSent     += static_cast<size_t>(n);
            transferred += static_cast<uint64_t>(n);
        }
    }
};

// ---------------------------------------------------------------------------
// SftpService
// ---------------------------------------------------------------------------

SftpService::SftpService(_LIBSSH2_SESSION*& session, const std::atomic<bool>& running)
    : session_(session), running_(running)
{}

SftpService::InitResult SftpService::EnsureSftp(FsError& err)
{
    if (sftp_) return InitResult::Ready;

    if (LIBSSH2_SFTP* s = libssh2_sftp_init(session_)) {
        sftp_ = s;
        initDeadline_.reset();
        return InitResult::Ready;
    }

    if (libssh2_session_last_errno(session_) != LIBSSH2_ERROR_EAGAIN) {
        initDeadline_.reset();
        err = FsError::Make(FsErrorCode::Unsupported,
                            "SFTP unavailable: " + ssh::LastSshError(session_));
        return InitResult::Failed;
    }

    // EAGAIN: arm the deadline on first sight, then fail once it elapses.
    const auto now = std::chrono::steady_clock::now();
    if (!initDeadline_) initDeadline_ = now + kSftpInitTimeout;
    if (now >= *initDeadline_) {
        initDeadline_.reset();
        err = FsError::Make(FsErrorCode::Unsupported,
                            "SFTP unavailable: timed out starting the SFTP subsystem "
                            "(is sftp-server installed on the remote?)");
        return InitResult::Failed;
    }
    return InitResult::Pending;
}

FsError SftpService::MakeError(const std::string& context) const
{
    const std::string detail = ssh::LastSshError(session_);

    // The SFTP status code is only meaningful when libssh2 is reporting a
    // protocol-level failure; at any other time it holds a stale value from a
    // previous operation.
    if (sftp_ &&
        libssh2_session_last_errno(session_) == LIBSSH2_ERROR_SFTP_PROTOCOL) {
        const unsigned long status = libssh2_sftp_last_error(sftp_);
        return FsError::Make(ClassifySftpStatus(status), context + ": " + detail);
    }
    return FsError::Make(FsErrorCode::Protocol, context + ": " + detail);
}

void SftpService::Service()
{
    queue_.RunNext();
}

void SftpService::CancelPending()
{
    // Latched before the queue is taken, so every task below — and anything
    // enqueued while this runs — sees the stop on its first check and retires
    // in one step rather than asking to be resumed.
    cancelling_.store(true, std::memory_order_release);

    for (auto& task : queue_.Drain())
        task.step();  // sees Stopped(), reports NotConnected, returns false
}

void SftpService::Shutdown()
{
    if (sftp_) {
        libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
    }
}

void SftpService::RegisterTransfer(TransferHandle handle)
{
    if (handle == kInvalidTransferHandle) return;
    std::lock_guard<std::mutex> lk(cancelMutex_);
    live_.insert(handle);
}

bool SftpService::IsCancelled(TransferHandle handle) const
{
    if (handle == kInvalidTransferHandle) return false;
    std::lock_guard<std::mutex> lk(cancelMutex_);
    return cancelled_.count(handle) != 0;
}

void SftpService::ForgetCancellation(TransferHandle handle)
{
    if (handle == kInvalidTransferHandle) return;
    std::lock_guard<std::mutex> lk(cancelMutex_);
    live_.erase(handle);
    cancelled_.erase(handle);
}

void SftpService::Cancel(TransferHandle handle)
{
    if (handle == kInvalidTransferHandle) return;
    std::lock_guard<std::mutex> lk(cancelMutex_);
    // Only a transfer that is still running can be asked to stop. A handle that
    // has already finished — or never existed — is dropped here rather than
    // recorded, because nothing will come back to clear it. The port promises
    // both calls are harmless, and this is what makes the second one so.
    if (live_.count(handle) == 0) return;
    cancelled_.insert(handle);
}

// ---------------------------------------------------------------------------
// IRemoteFileSystem
// ---------------------------------------------------------------------------

void SftpService::List(const std::string& path, ListCallback onDone)
{
    auto t = std::make_shared<ListDirTask>();
    t->svc    = this;
    t->path   = path;
    t->onDone = std::move(onDone);
    EnqueueTask(t);
}

void SftpService::RealPath(const std::string& path, PathCallback onDone)
{
    auto t = std::make_shared<PathOpTask>();
    t->svc     = this;
    t->path    = path;
    t->flag    = LIBSSH2_SFTP_REALPATH;
    t->context = "Cannot resolve '" + path + "'";
    t->onDone  = std::move(onDone);
    EnqueueTask(t);
}

void SftpService::ReadLink(const std::string& path, PathCallback onDone)
{
    auto t = std::make_shared<PathOpTask>();
    t->svc     = this;
    t->path    = path;
    t->flag    = LIBSSH2_SFTP_READLINK;
    t->context = "Cannot read link '" + path + "'";
    t->onDone  = std::move(onDone);
    EnqueueTask(t);
}

void SftpService::Stat(const std::string& path, StatCallback onDone)
{
    auto t = std::make_shared<StatTask>();
    t->svc    = this;
    t->path   = path;
    t->onDone = std::move(onDone);
    EnqueueTask(t);
}

void SftpService::MakeDirectory(const std::string& path, uint32_t mode,
                                DoneCallback onDone)
{
    auto t = std::make_shared<SimpleOpTask>();
    t->svc     = this;
    t->opSlot  = Slot::MkDir;
    t->context = "Cannot create directory '" + path + "'";
    t->onDone  = std::move(onDone);
    t->op = [path, mode](SimpleOpTask& self) {
        return libssh2_sftp_mkdir_ex(self.svc->sftp_, path.c_str(),
                                     static_cast<unsigned>(path.size()),
                                     static_cast<long>(mode));
    };
    EnqueueTask(t);
}

void SftpService::Remove(const std::string& path, bool isDir, DoneCallback onDone)
{
    auto t = std::make_shared<SimpleOpTask>();
    t->svc     = this;
    t->opSlot  = isDir ? Slot::RmDir : Slot::Unlink;
    t->context = std::string("Cannot delete ") + (isDir ? "directory '" : "'") +
                 path + "'";
    t->onDone  = std::move(onDone);
    t->op = [path, isDir](SimpleOpTask& self) {
        const auto len = static_cast<unsigned>(path.size());
        return isDir ? libssh2_sftp_rmdir_ex(self.svc->sftp_, path.c_str(), len)
                     : libssh2_sftp_unlink_ex(self.svc->sftp_, path.c_str(), len);
    };
    EnqueueTask(t);
}

void SftpService::CreateSymlink(const std::string& target,
                                const std::string& linkPath,
                                DoneCallback onDone)
{
    auto t = std::make_shared<SimpleOpTask>();
    t->svc     = this;
    t->opSlot  = Slot::Symlink;
    t->context = "Cannot create link '" + linkPath + "'";
    t->onDone  = std::move(onDone);
    // Same entry point as realpath and readlink, third flag value. Note the
    // argument roles differ from those two: here `target` is an input, not an
    // output buffer, which is why this is a SimpleOpTask rather than a
    // PathOpTask. The const_cast is libssh2's signature, not our intent — the
    // parameter is char* for the readlink case and is only read for this one.
    t->op = [target, linkPath](SimpleOpTask& self) {
        return libssh2_sftp_symlink_ex(
            self.svc->sftp_,
            target.c_str(),   static_cast<unsigned>(target.size()),
            const_cast<char*>(linkPath.c_str()),
            static_cast<unsigned>(linkPath.size()),
            LIBSSH2_SFTP_SYMLINK);
    };
    EnqueueTask(t);
}

void SftpService::Rename(const std::string& from, const std::string& to,
                         DoneCallback onDone)
{
    auto t = std::make_shared<SimpleOpTask>();
    t->svc     = this;
    t->opSlot  = Slot::Rename;
    t->context = "Cannot rename '" + from + "' to '" + to + "'";
    t->onDone  = std::move(onDone);
    // No OVERWRITE flag: a rename that would clobber an existing file must
    // fail so the caller can ask, rather than destroying data silently.
    t->op = [from, to](SimpleOpTask& self) {
        return libssh2_sftp_rename_ex(self.svc->sftp_,
                                      from.c_str(), static_cast<unsigned>(from.size()),
                                      to.c_str(),   static_cast<unsigned>(to.size()),
                                      LIBSSH2_SFTP_RENAME_ATOMIC |
                                      LIBSSH2_SFTP_RENAME_NATIVE);
    };
    EnqueueTask(t);
}

void SftpService::SetPermissions(const std::string& path, uint32_t mode,
                                 DoneCallback onDone)
{
    auto t = std::make_shared<SimpleOpTask>();
    t->svc               = this;
    // setstat and stat are the same libssh2 state machine.
    t->opSlot            = Slot::Stat;
    t->context           = "Cannot set permissions on '" + path + "'";
    t->attrs.flags       = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    t->attrs.permissions = mode;
    t->onDone            = std::move(onDone);
    t->op = [path](SimpleOpTask& self) {
        return libssh2_sftp_stat_ex(self.svc->sftp_, path.c_str(),
                                    static_cast<unsigned>(path.size()),
                                    LIBSSH2_SFTP_SETSTAT, &self.attrs);
    };
    EnqueueTask(t);
}

TransferHandle SftpService::Download(const std::string& remotePath,
                                     const std::string& localPath,
                                     ProgressCallback onProgress,
                                     DoneCallback onDone)
{
    const TransferHandle id = nextHandle_.fetch_add(1, std::memory_order_relaxed);
    // Registered before the task is queued, so a Cancel() racing this call is
    // recorded rather than discarded as unknown.
    RegisterTransfer(id);
    auto t = std::make_shared<DownloadTask>();
    t->svc        = this;
    t->handleId   = id;
    t->remotePath = remotePath;
    t->localPath  = localPath;
    t->onProgress = std::move(onProgress);
    t->onDone     = std::move(onDone);
    EnqueueTask(t);
    return id;
}

TransferHandle SftpService::Upload(const std::string& localPath,
                                   const std::string& remotePath,
                                   ProgressCallback onProgress,
                                   DoneCallback onDone)
{
    const TransferHandle id = nextHandle_.fetch_add(1, std::memory_order_relaxed);
    RegisterTransfer(id);
    auto t = std::make_shared<UploadTask>();
    t->svc        = this;
    t->handleId   = id;
    t->localPath  = localPath;
    t->remotePath = remotePath;
    t->onProgress = std::move(onProgress);
    t->onDone     = std::move(onDone);
    EnqueueTask(t);
    return id;
}

} // namespace term::transport
