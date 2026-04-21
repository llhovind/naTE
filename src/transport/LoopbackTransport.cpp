#include "transport/LoopbackTransport.h"

namespace term::transport
{

    void LoopbackTransport::Write(const std::string &data)
    {
        if (callback_)
        {
            callback_(data); // echo back immediately
        }
    }

    void LoopbackTransport::SetReadCallback(DataCallback cb)
    {
        callback_ = std::move(cb);
    }

}
