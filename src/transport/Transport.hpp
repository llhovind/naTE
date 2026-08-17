#pragma once
#include "transport/PortForward.h"
#include <string>

namespace term::transport {

class IRemoteFileSystem;

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

    virtual bool SupportsX11Forwarding()   const noexcept { return false; }
    virtual bool SupportsPortForwarding()  const noexcept { return false; }

    virtual void AddPortForward(const PortForwardDesc& /*desc*/) {}
    virtual void RemovePortForward(PortForwardId /*id*/) {}
    virtual std::string GetRemoteDescription() const { return {}; }

    // The transport's view of the remote filesystem, or nullptr when it has
    // none — which is the capability check, so there is no separate
    // SupportsFileTransfer() to fall out of sync with it.
    //
    // The returned object is owned by the transport and dies with it. Callers
    // must not retain it across a session teardown. See IRemoteFileSystem for
    // the threading contract.
    virtual IRemoteFileSystem* GetRemoteFileSystem() { return nullptr; }
};

} // namespace term::transport
