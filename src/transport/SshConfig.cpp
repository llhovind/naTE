#include "transport/SshConfig.h"
#include <cstdio>
#include <cstdlib>
#include <optional>
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

// Returns user if non-empty, otherwise the current OS login name ($USER then
// $LOGNAME).  When both environment variables are unset an empty string is
// returned and the caller omits the -l flag so that ssh uses its own default.
std::string EffectiveUser(const std::string& user)
{
    if (!user.empty()) return user;
    const char* u = std::getenv("USER");
    if (!u || *u == '\0') u = std::getenv("LOGNAME");
    return u ? u : "";
}

void AppendUserFlag(std::string& cmd, const std::string& user)
{
    const std::string effective = EffectiveUser(user);
    if (!effective.empty()) {
        cmd += " -l ";
        cmd += effective;
    }
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
    AppendUserFlag(cmd, user);
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

std::optional<term::session::ProxyJumpDesc> QuerySshConfigProxyJump(
    const std::string& host,
    uint16_t           port,
    const std::string& user,
    const std::string& configPath)
{
    std::string cmd = "ssh -G";
    if (!configPath.empty()) {
        cmd += " -F ";
        cmd += configPath;
    }
    cmd += " -p ";
    cmd += std::to_string(port);
    AppendUserFlag(cmd, user);
    cmd += " -- ";
    cmd += host;
    cmd += " 2>/dev/null";

    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return std::nullopt;

    std::optional<term::session::ProxyJumpDesc> result;
    char line[4096];
    while (std::fgets(line, sizeof(line), pipe)) {
        std::string_view sv(line);
        constexpr std::string_view kPrefix = "proxyjump ";
        if (sv.substr(0, kPrefix.size()) != kPrefix) continue;

        sv.remove_prefix(kPrefix.size());
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
            sv.remove_suffix(1);

        // "none" means no proxy — treat as absent
        if (sv.empty() || sv == "none") break;

        // Only use the first hop if comma-separated (multi-hop not yet supported)
        const auto commaPos = sv.find(',');
        if (commaPos != std::string_view::npos)
            sv = sv.substr(0, commaPos);

        term::session::ProxyJumpDesc jump;

        // Parse optional user@ prefix
        const auto atPos = sv.rfind('@');
        if (atPos != std::string_view::npos) {
            jump.user = std::string(sv.substr(0, atPos));
            sv.remove_prefix(atPos + 1);
        }

        // Parse optional :port suffix
        // Use rfind to correctly handle IPv6 addresses in brackets, e.g. [::1]:22
        const auto colonPos = sv.rfind(':');
        if (colonPos != std::string_view::npos) {
            const std::string_view portSv = sv.substr(colonPos + 1);
            // Ensure it looks like a numeric port, not part of an IPv6 literal
            bool allDigits = !portSv.empty();
            for (char c : portSv) allDigits = allDigits && (c >= '0' && c <= '9');
            if (allDigits) {
                jump.port = static_cast<unsigned short>(std::stoi(std::string(portSv)));
                sv = sv.substr(0, colonPos);
            }
        }

        // Strip surrounding brackets from IPv6 literals
        if (sv.size() >= 2 && sv.front() == '[' && sv.back() == ']')
            sv = sv.substr(1, sv.size() - 2);

        jump.host = std::string(sv);
        if (!jump.host.empty())
            result = std::move(jump);
        break;
    }
    ::pclose(pipe);
    return result;
}

ResolvedSshHost ResolveHostName(
    const std::string& alias,
    uint16_t           port,
    const std::string& user,
    const std::string& configPath)
{
    ResolvedSshHost out{alias, port};

    std::string cmd = "ssh -G";
    if (!configPath.empty()) {
        cmd += " -F ";
        cmd += configPath;
    }
    cmd += " -p ";
    cmd += std::to_string(port);
    AppendUserFlag(cmd, user);
    cmd += " -- ";
    cmd += alias;
    cmd += " 2>/dev/null";

    std::FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return out;

    bool gotHostname = false, gotPort = false;
    char line[4096];
    while ((!gotHostname || !gotPort) && std::fgets(line, sizeof(line), pipe)) {
        std::string_view sv(line);
        while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r'))
            sv.remove_suffix(1);

        constexpr std::string_view kHost = "hostname ";
        constexpr std::string_view kPort = "port ";
        if (!gotHostname && sv.substr(0, kHost.size()) == kHost) {
            out.hostname = std::string(sv.substr(kHost.size()));
            gotHostname  = true;
        } else if (!gotPort && sv.substr(0, kPort.size()) == kPort) {
            const std::string_view pv = sv.substr(kPort.size());
            bool allDigits = !pv.empty();
            for (char c : pv) allDigits = allDigits && (c >= '0' && c <= '9');
            if (allDigits) {
                out.port = static_cast<uint16_t>(std::stoi(std::string(pv)));
                gotPort  = true;
            }
        }
    }
    ::pclose(pipe);
    return out;
}

} // namespace term::transport
