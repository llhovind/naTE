#pragma once
#include "transport/Transport.hpp"   // RemoteDirEntry

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

namespace term::transport {

// Owns the lazily-initialised SFTP subsystem and the cooperative task queue.
// File operations are submitted from the UI thread (enqueue methods touch only
// the queue under queueMutex_) and advanced one non-blocking step per
// worker-loop iteration via Service(). Everything else is worker-thread-only.
//
// The session pointer is bound by reference: the transport creates the libssh2
// session on the worker thread after this object is constructed, so the service
// reads the live value rather than a stale snapshot.
class SftpService {
public:
    SftpService(_LIBSSH2_SESSION*& session, const std::atomic<bool>& running);

    SftpService(const SftpService&)            = delete;
    SftpService& operator=(const SftpService&) = delete;

    // --- UI-thread enqueue API (1:1 with the Transport SFTP methods) ---------
    // SendFile/ReceiveFile derive the leaf filename and join it to the target
    // directory; the *ToPath / *FromPath variants take exact remote/local paths.
    void SendFile(const std::string& localPath, const std::string& remoteDir,
                  std::function<void(bool, std::string)> onDone);
    void ReceiveFile(const std::string& remotePath, const std::string& localDir,
                     std::function<void(bool, std::string)> onDone);
    void ListRemoteDirectory(
        const std::string& remotePath,
        std::function<void(std::vector<RemoteDirEntry>, std::string)> onDone);
    void DownloadToPath(const std::string& remotePath, const std::string& localPath,
                        std::function<void(bool, std::string)> onDone);
    void UploadFromPath(const std::string& localPath, const std::string& remotePath,
                        std::function<void(bool, std::string)> onDone);

    // --- Worker-thread API ---------------------------------------------------
    // Advances the front task by one step; re-queues it if it returns true.
    void Service();
    // Drains the queue, invoking each task once so it self-cancels (sees
    // !running_ and fires onDone(false, "Session closed")). Call after clearing
    // running_ during teardown.
    void CancelPending();
    // libssh2_sftp_shutdown if initialised; idempotent. Safe to call twice
    // (orderly path then dead-socket fallback), matching the prior teardown.
    void Shutdown();

private:
    struct ListDirTask;
    struct DownloadTask;
    struct UploadTask;

    using Task = std::function<bool()>;

    _LIBSSH2_SESSION*&       session_;
    const std::atomic<bool>& running_;
    _LIBSSH2_SFTP*           sftp_ = nullptr;
    std::mutex               queueMutex_;
    std::deque<Task>         queue_;
};

} // namespace term::transport
