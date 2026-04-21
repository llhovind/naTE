#include "transport/LoopbackTransport.h"

namespace term::transport
{

    void LoopbackTransport::Write(const std::string &data)
    {
        if (!callback_)
            return;

        // Simulate tty line discipline: bare CR (\r not followed by \n) → CRLF.
        std::string translated;
        translated.reserve(data.size());
        for (size_t i = 0; i < data.size(); ++i) {
            translated += data[i];
            if (data[i] == '\r' && (i + 1 >= data.size() || data[i + 1] != '\n'))
                translated += '\n';
        }
        callback_(translated);
    }

    void LoopbackTransport::SetReadCallback(DataCallback cb)
    {
        callback_ = std::move(cb);
    }

}
