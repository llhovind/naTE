#pragma once

#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"

namespace term::transport {

class LoopbackTransport : public Transport {
public:
    explicit LoopbackTransport(ITransportTarget& target);

    void Write(const std::string& data) override;
    void Start() override {}
    void Resize(unsigned short /*cols*/, unsigned short /*rows*/) override {}

private:
    ITransportTarget& target_;
};

} // namespace term::transport
