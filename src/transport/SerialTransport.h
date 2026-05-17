#pragma once
#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"

#include <atomic>
#include <thread>

namespace term::transport {

class SerialTransport final : public Transport {
public:
    SerialTransport(ITransportTarget& target,
                    const term::session::SerialDesc& desc,
                    const term::session::SessionInit& sessionInit = {},
                    const term::session::AppSessionDefaults& appDefaults = {});
    ~SerialTransport() override;

    void Write(const std::string& data) override;
    void Start() override;
    void Stop()  override;
    void Resize(unsigned short cols, unsigned short rows) override;

private:
    void ConfigureTty();
    void RunDialScript();
    void ReadLoop();

    ITransportTarget&                  target_;
    const term::session::SerialDesc    desc_;
    term::session::SessionInit         sessionInit_;
    term::session::AppSessionDefaults  appDefaults_;
    int                                fd_      = -1;
    std::atomic<bool>                  running_ { false };
    std::thread                        reader_;
};

} // namespace term::transport
