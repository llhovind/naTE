#pragma once
#include <functional>
#include <string>

namespace term::transport {

class Transport {
public:
    virtual ~Transport() = default;

    virtual void Write(const std::string& data) = 0;
    virtual void Start() = 0;
    virtual void Stop() {}
    virtual void Resize(unsigned short cols, unsigned short rows) = 0;
    virtual void OnViewportColsChanged(unsigned short /*cols*/) {}

    virtual bool SupportsFileTransfer() const noexcept { return false; }
    virtual std::string GetRemoteDescription() const { return {}; }

    // Transfers localPath into remoteDir via SCP. onDone is invoked on the UI
    // thread (via wxTheApp->CallAfter). Default no-op for PTY and Serial.
    virtual void TransferFile(
        const std::string& /*localPath*/,
        const std::string& /*remoteDir*/,
        std::function<void(bool success, std::string error)> /*onDone*/) {}
};

} // namespace term::transport
