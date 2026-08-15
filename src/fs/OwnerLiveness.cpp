#include "fs/OwnerLiveness.h"

#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

namespace term::fs {

namespace {

// Reads a process's name from /proc, or nullopt when there is no such process.
std::optional<std::string> ProcessName(int pid)
{
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    if (!in) return std::nullopt;

    std::string name;
    if (!std::getline(in, name)) return std::nullopt;
    return name;
}

} // namespace

bool DefaultOwnerIsLive(int pid)
{
    if (pid <= 0) return false;

    // Nothing of this run can have written such a file yet, so one wearing our
    // pid belongs to a dead instance whose number we inherited.
    if (pid == ::getpid()) return false;

    const auto owner = ProcessName(pid);
    if (!owner) return false;

    const auto self = ProcessName(::getpid());
    return self && *owner == *self;
}

} // namespace term::fs
