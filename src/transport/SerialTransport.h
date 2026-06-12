#pragma once
#include "transport/AppSessionDefaults.h"
#include "transport/TransportDesc.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"

#include <atomic>
#include <thread>

namespace term::transport {

class SerialTransport final : public Transport {
public:
    SerialTransport(ITransportTarget& target,
                    const term::transport::SerialDesc& desc,
                    const term::transport::SessionInit& sessionInit = {},
                    const term::transport::AppSessionDefaults& appDefaults = {});
    ~SerialTransport() override;

    void Write(const std::string& data) override;
    void SendResetSequence() override { Write("\021"); }  // XON only; see SshTransport.h
    void Start() override;
    void Stop()  override;
    void Resize(unsigned short cols, unsigned short rows) override;

private:
    void ConfigureTty();
    void RunDialScript();
    void ReadLoop();

    ITransportTarget&                  target_;
    const term::transport::SerialDesc    desc_;
    term::transport::SessionInit         sessionInit_;
    term::transport::AppSessionDefaults  appDefaults_;
    int                                fd_      = -1;
    std::atomic<bool>                  running_ { false };
    std::thread                        reader_;
};

} // namespace term::transport
