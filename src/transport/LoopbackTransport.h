#pragma once

#include "Transport.hpp"

namespace term::transport
{

    class LoopbackTransport : public Transport
    {
    public:
        void Write(const std::string &data) override;
        void SetReadCallback(DataCallback cb) override;

    private:
        DataCallback callback_;
    };

}
