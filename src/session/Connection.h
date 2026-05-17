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

struct Connection {
    std::string    label;
    TransportDesc  transport;
    bool           wrapMode    = false;
    unsigned short columnWidth = 0;   // 0 = use app config default

    // Returns the hard-coded set of available connection templates.
    static std::vector<Connection> Defaults(const std::string& shell);
};

} // namespace term::session
