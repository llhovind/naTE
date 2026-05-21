#pragma once
#include "session/Connection.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace term::transport {

// Returns the IdentityFile paths from the effective SSH config for the given
// host/port/user, in the order OpenSSH would try them. Delegates to `ssh -G`
// so all config features (Host wildcards, Include, Match, system config) are
// handled correctly.
//
// configPath: if non-empty, passed as -F to ssh — used in tests to supply a
//             synthetic config without touching ~/.ssh/config.
//
// Returns an empty vector on any failure (ssh not found, popen error, etc.).
std::vector<std::filesystem::path> QuerySshConfigIdentities(
    const std::string& host,
    uint16_t           port,
    const std::string& user,
    const std::string& configPath = {}
);

// Returns the ProxyJump descriptor from the effective SSH config for the given
// target host/port/user, or nullopt if not configured or set to "none".
// Delegates to `ssh -G`; parses `[user@]host[:port]` from the proxyjump line.
// Only single-hop ProxyJump is supported; chained values (host1,host2) are
// treated as the first hop only.
//
// configPath: if non-empty, passed as -F to ssh — used in tests.
std::optional<term::session::ProxyJumpDesc> QuerySshConfigProxyJump(
    const std::string& host,
    uint16_t           port,
    const std::string& user,
    const std::string& configPath = {}
);

} // namespace term::transport
