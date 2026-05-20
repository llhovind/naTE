#include "transport/SshConfig.h"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace term::transport {

namespace {

std::string ExpandTilde(const std::string& path)
{
    if (path.size() >= 2 && path[0] == '~' && path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home) return std::string(home) + path.substr(1);
    }
    return path;
}

} // namespace

std::vector<std::filesystem::path> QuerySshConfigIdentities(
    const std::string& host,
    uint16_t           port,
    const std::string& user,
    const std::string& configPath)
{
    // Build: ssh -G [-F configPath] -p PORT -l USER -- HOST
    std::string cmd = "ssh -G";
    if (!configPath.empty()) {
        cmd += " -F ";
        cmd += configPath;
    }
    cmd += " -p ";
    cmd += std::to_string(port);
    cmd += " -l ";
    cmd += user;
    cmd += " -- ";
    cmd += host;
    cmd += " 2>/dev/null";

    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return {};

    std::vector<std::filesystem::path> result;
    char line[4096];
    while (std::fgets(line, sizeof(line), pipe)) {
        std::string_view sv(line);
        // ssh -G lowercases all keys; values are already tilde-expanded by OpenSSH
        // on most platforms, but we expand defensively.
        constexpr std::string_view kPrefix = "identityfile ";
        if (sv.substr(0, kPrefix.size()) != kPrefix) continue;

        sv.remove_prefix(kPrefix.size());
        // Strip trailing newline
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
            sv.remove_suffix(1);

        if (sv.empty()) continue;
        result.emplace_back(ExpandTilde(std::string(sv)));
    }
    ::pclose(pipe);
    return result;
}

} // namespace term::transport
