#pragma once
#include <string>
#include <variant>
#include <vector>

namespace term::session {

struct PtyDesc      { std::string shell; };
struct LoopbackDesc {};

enum class SshAuthMethod { Agent, Password, PrivateKey };

struct SshDesc {
    std::string    host;
    unsigned short port              = 22;
    std::string    username;
    SshAuthMethod  authMethod        = SshAuthMethod::Agent;
    std::string    password;
    std::string    privateKeyPath;
    std::string    publicKeyPath;     // derived from privateKeyPath if empty
    std::string    passphrase;
    int            keepaliveSeconds  = 30;   // 0 = disabled
    int            connectTimeoutSec = 10;
    std::string    remoteCommand;     // empty = login shell
    bool           compress          = false;
};

using TransportDesc = std::variant<PtyDesc, LoopbackDesc, SshDesc>;

struct Connection {
    std::string   label;
    TransportDesc transport;

    // Returns the hard-coded set of available connection templates.
    static std::vector<Connection> Defaults(const std::string& shell);
};

} // namespace term::session
