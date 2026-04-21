#pragma once

#include <functional>
#include <string>

namespace term::transport
{

    class Transport
    {
    public:
        using DataCallback = std::function<void(const std::string &)>;

        virtual ~Transport() = default;

        virtual void Write(const std::string &data) = 0;
        virtual void SetReadCallback(DataCallback cb) = 0;
    };

} // namespace
