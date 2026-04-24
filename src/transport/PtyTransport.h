#pragma once

#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include <atomic>
#include <string>
#include <thread>
#include <sys/types.h>

namespace term::transport {

class PtyTransport : public Transport {
public:
    PtyTransport(ITransportTarget& target, std::string shell,
                 unsigned short cols, unsigned short rows);
    ~PtyTransport() override;

    PtyTransport(const PtyTransport&) = delete;
    PtyTransport& operator=(const PtyTransport&) = delete;

    void Write(const std::string& data) override;
    void Start() override;
    void Stop() override;
    void Resize(unsigned short cols, unsigned short rows) override;

private:
    void ReadLoop();

    ITransportTarget& target_;
    std::string       shell_;
    int               master_fd_ = -1;
    pid_t             child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread       reader_;
};

} // namespace term::transport
