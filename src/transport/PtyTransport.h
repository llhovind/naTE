#pragma once

#include "Transport.hpp"
#include <atomic>
#include <string>
#include <thread>
#include <sys/types.h>

namespace term::transport
{

class PtyTransport : public Transport
{
public:
    PtyTransport(std::string shell, unsigned short cols, unsigned short rows);
    ~PtyTransport() override;

    PtyTransport(const PtyTransport&) = delete;
    PtyTransport& operator=(const PtyTransport&) = delete;

    void Write(const std::string& data) override;
    void SetReadCallback(DataCallback cb) override;

private:
    void ReadLoop();

    std::string       shell_;
    int               master_fd_ = -1;
    pid_t             child_pid_ = -1;
    DataCallback      callback_;
    std::atomic<bool> running_{false};
    std::thread       reader_;
};

} // namespace term::transport
