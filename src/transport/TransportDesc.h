#pragma once
// Transport descriptors — the configuration each transport backend is built
// from. These are transport-domain types: session/ composes them into a
// Connection, but the dependency points one way (session → transport).
#include "transport/EnvVar.h"
#include "transport/PortForward.h"
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace term::transport {

struct SessionInit {
    std::string         workingDir;    // empty = inherit parent cwd; ~ expanded at spawn time
    std::vector<EnvVar> envVars;       // explicit KEY=VALUE overrides applied to the child process
    std::string         envFilePath;   // path to a .env file to load; empty = none; ~ expanded
    bool                loginShell = false; // prefix argv[0] with '-' (PTY) or use exec -l (SSH) to source .profile
};

struct PtyDesc {
    std::string shell;
    std::string command; // empty = interactive shell; non-empty = exec shell -c command
};
struct LoopbackDesc {};

enum class SshAuthMethod { Agent, Password, PrivateKey, KbdInteractive };

struct ProxyJumpDesc {
    std::string    host;
    unsigned short port              = 22;
    std::string    user;            // empty = same as SshDesc::username
    SshAuthMethod  authMethod       = SshAuthMethod::Agent;
    std::string    password;        // not persisted — matches SshDesc convention
    std::string    privateKeyPath;
    std::string    publicKeyPath;   // derived from privateKeyPath if empty
    std::string    passphrase;      // not persisted
    std::string    agentIdentityHint;
};

struct SshDesc {
    std::string    host;
    unsigned short port              = 22;
    std::string    username;
    SshAuthMethod  authMethod        = SshAuthMethod::Agent;
    std::string    password;
    std::string    privateKeyPath;
    std::string    publicKeyPath;     // derived from privateKeyPath if empty
    std::string    passphrase;
    int            connectTimeoutSec = 10;
    std::string    remoteCommand;     // empty = login shell
    bool           compress          = false;
    bool           x11Forwarding     = false; // request X11 forwarding at channel open
    bool           agentForwarding   = false; // request SSH agent forwarding at channel open
    std::string    agentIdentityHint; // optional path to private key (or .pub) preferred when using agent auth; empty = consult ~/.ssh/config
    std::optional<ProxyJumpDesc>   proxyJump;    // nullopt = direct TCP connection
    std::vector<PortForwardDesc>   portForwards; // persisted; IDs stripped on save
};

enum class SerialParity      { None, Even, Odd };
enum class SerialFlowControl { None, Hardware, Software };
enum class SerialStopBits    { One, Two };

struct SerialDesc {
    std::string      device;                                // e.g. "/dev/ttyUSB0"
    unsigned int     baudRate    = 115200;
    unsigned short   dataBits    = 8;                       // 5 | 6 | 7 | 8
    SerialStopBits   stopBits    = SerialStopBits::One;
    SerialParity     parity      = SerialParity::None;
    SerialFlowControl flowControl = SerialFlowControl::None;
    std::string      dialScript;                            // empty = raw serial; path to executable run before I/O
};

using TransportDesc = std::variant<PtyDesc, LoopbackDesc, SshDesc, SerialDesc>;

} // namespace term::transport
