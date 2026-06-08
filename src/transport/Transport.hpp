#pragma once
#include "transport/PortForward.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace term::transport {

struct RemoteDirEntry {
    std::string name;
    std::string permissions;
    std::string modTime;
    uint64_t    size      = 0;
    bool        isDir     = false;
    bool        isSymlink = false;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual void Write(const std::string& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() {}
    virtual void Resize(unsigned short cols, unsigned short rows) = 0;
    virtual void OnViewportColsChanged(unsigned short /*cols*/) {}

    // Sends ESC c (RIS) to re-sync the remote terminal's parser state.
    // Only meaningful for transports with a remote terminal (SSH, Serial).
    // PTY and Loopback are no-ops: the local shell sees raw keyboard input, not
    // terminal control, so ESC c would be echoed as a stray 'c' by some shells
    // (e.g. ash/busybox) instead of being silently consumed.
    virtual void SendResetSequence() {}

    // Enqueues an X11 forwarding request on the active channel; no-op for non-SSH transports.
    virtual void RequestX11Forwarding() {}

    // Returns the current working directory of the transport's child process, if
    // accessible. PTY transports read this from /proc/<pid>/cwd; all others return "".
    virtual std::string GetCurrentWorkingDir() const { return {}; }

    virtual bool SupportsFileTransfer()    const noexcept { return false; }
    virtual bool SupportsX11Forwarding()   const noexcept { return false; }
    virtual bool SupportsPortForwarding()  const noexcept { return false; }

    virtual void AddPortForward(const PortForwardDesc& /*desc*/) {}
    virtual void RemovePortForward(PortForwardId /*id*/) {}
    virtual std::string GetRemoteDescription() const { return {}; }

    // Uploads localPath into remoteDir via SFTP. onDone is invoked on the
    // calling thread; callers are responsible for marshalling to the UI thread
    // if required (e.g. wxTheApp->CallAfter). Default no-op for PTY/Serial.
    virtual void SendFile(
        const std::string& /*localPath*/,
        const std::string& /*remoteDir*/,
        std::function<void(bool success, std::string error)> /*onDone*/) {}

    // Downloads remotePath into localDir via SFTP. onDone calling convention
    // matches SendFile above.
    virtual void ReceiveFile(
        const std::string& /*remotePath*/,
        const std::string& /*localDir*/,
        std::function<void(bool success, std::string error)> /*onDone*/) {}

    // Lists the contents of remotePath via SFTP. onDone calling convention
    // matches SendFile above.
    virtual void ListRemoteDirectory(
        const std::string& /*remotePath*/,
        std::function<void(std::vector<RemoteDirEntry>, std::string error)> /*onDone*/) {}

    // Downloads remotePath to the exact local file path localPath via SFTP.
    // Used by the remote-edit workflow (PR 2). Default no-op.
    virtual void SftpDownloadFile(
        const std::string& /*remotePath*/,
        const std::string& /*localPath*/,
        std::function<void(bool success, std::string localPathOrError)> /*onDone*/) {}

    // Uploads localPath to the exact remote file path remotePath via SFTP.
    // Used by the remote-edit auto-upload workflow (PR 2). Default no-op.
    virtual void SftpUploadFile(
        const std::string& /*localPath*/,
        const std::string& /*remotePath*/,
        std::function<void(bool success, std::string error)> /*onDone*/) {}
};

} // namespace term::transport
