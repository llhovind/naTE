#pragma once

#include "session/AppSessionDefaults.h"
#include "session/Connection.h"
#include "transport/Transport.hpp"
#include "transport/ITransportTarget.h"
#include <atomic>
#include <string>
#include <thread>
#include <sys/types.h>

namespace term::transport {

class PtyTransport : public Transport {
public:
    PtyTransport(ITransportTarget& target,
                 std::string shell,
                 std::string command,
                 unsigned short cols,
                 unsigned short rows,
                 unsigned short viewportCols,
                 const term::session::SessionInit& sessionInit = {},
                 const term::session::AppSessionDefaults& appDefaults = {});
    ~PtyTransport() override;

    PtyTransport(const PtyTransport&) = delete;
    PtyTransport& operator=(const PtyTransport&) = delete;

    void        Write(const std::string& data) override;
    void        Start() override;
    void        Stop() override;
    void        Resize(unsigned short cols, unsigned short rows) override;
    void        OnViewportColsChanged(unsigned short cols) override;
    std::string GetCurrentWorkingDir() const override;

private:
    void ReadLoop();
    static void WriteVpColumns(const std::string& path, unsigned short cols);

    ITransportTarget& target_;
    std::string       shell_;
    std::string       command_;
    std::string       vpcolumns_file_;
    int               master_fd_ = -1;
    pid_t             child_pid_ = -1;
    std::atomic<bool> running_{false};
    std::thread       reader_;
};

} // namespace term::transport
