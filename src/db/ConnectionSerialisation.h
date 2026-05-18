#pragma once
// Internal header: shared serialisation helpers for Connection transport and
// SessionInit fields. Included by JsonConnectionRepository.cpp and
// JsonSessionRestoreRepository.cpp — not part of the public API.

#include "session/Connection.h"
#include <nlohmann/json.hpp>
#include <string>

namespace term::db::serialisation {

using json = nlohmann::json;

inline std::string AuthMethodToString(term::session::SshAuthMethod m)
{
    switch (m) {
        case term::session::SshAuthMethod::Password:   return "password";
        case term::session::SshAuthMethod::PrivateKey: return "privatekey";
        default:                                        return "agent";
    }
}

inline term::session::SshAuthMethod AuthMethodFromString(const std::string& s)
{
    if (s == "password")   return term::session::SshAuthMethod::Password;
    if (s == "privatekey") return term::session::SshAuthMethod::PrivateKey;
    return term::session::SshAuthMethod::Agent;
}

inline std::string ParityToString(term::session::SerialParity p)
{
    switch (p) {
        case term::session::SerialParity::Even: return "even";
        case term::session::SerialParity::Odd:  return "odd";
        default:                                return "none";
    }
}

inline term::session::SerialParity ParityFromString(const std::string& s)
{
    if (s == "even") return term::session::SerialParity::Even;
    if (s == "odd")  return term::session::SerialParity::Odd;
    return term::session::SerialParity::None;
}

inline std::string FlowToString(term::session::SerialFlowControl f)
{
    switch (f) {
        case term::session::SerialFlowControl::Hardware: return "hardware";
        case term::session::SerialFlowControl::Software: return "software";
        default:                                         return "none";
    }
}

inline term::session::SerialFlowControl FlowFromString(const std::string& s)
{
    if (s == "hardware") return term::session::SerialFlowControl::Hardware;
    if (s == "software") return term::session::SerialFlowControl::Software;
    return term::session::SerialFlowControl::None;
}

inline std::string StopBitsToString(term::session::SerialStopBits sb)
{
    return sb == term::session::SerialStopBits::Two ? "2" : "1";
}

inline term::session::SerialStopBits StopBitsFromString(const std::string& s)
{
    return s == "2" ? term::session::SerialStopBits::Two
                    : term::session::SerialStopBits::One;
}

inline json SerialiseTransport(const term::session::TransportDesc& transport)
{
    return std::visit([](const auto& desc) -> json {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, term::session::PtyDesc>) {
            return json{{"type", "pty"}, {"shell", desc.shell}};
        } else if constexpr (std::is_same_v<T, term::session::SshDesc>) {
            // passwords and passphrases are intentionally excluded
            return json{
                {"type",               "ssh"},
                {"host",               desc.host},
                {"port",               desc.port},
                {"username",           desc.username},
                {"authMethod",         AuthMethodToString(desc.authMethod)},
                {"privateKeyPath",     desc.privateKeyPath},
                {"publicKeyPath",      desc.publicKeyPath},
                {"keepaliveSeconds",   desc.keepaliveSeconds},
                {"connectTimeoutSec",  desc.connectTimeoutSec},
                {"remoteCommand",      desc.remoteCommand},
                {"compress",           desc.compress},
            };
        } else if constexpr (std::is_same_v<T, term::session::SerialDesc>) {
            return json{
                {"type",        "serial"},
                {"device",      desc.device},
                {"baudRate",    desc.baudRate},
                {"dataBits",    desc.dataBits},
                {"stopBits",    StopBitsToString(desc.stopBits)},
                {"parity",      ParityToString(desc.parity)},
                {"flowControl", FlowToString(desc.flowControl)},
                {"dialScript",  desc.dialScript},
            };
        } else {
            return json{{"type", "loopback"}};
        }
    }, transport);
}

inline term::session::TransportDesc DeserialiseTransport(const json& j)
{
    const std::string type = j.value("type", "loopback");

    if (type == "pty") {
        return term::session::PtyDesc{ j.value("shell", std::string{}) };
    }
    if (type == "ssh") {
        term::session::SshDesc d;
        d.host              = j.value("host",              std::string{});
        d.port              = j.value("port",              static_cast<unsigned short>(22));
        d.username          = j.value("username",          std::string{});
        d.authMethod        = AuthMethodFromString(j.value("authMethod", std::string{"agent"}));
        d.privateKeyPath    = j.value("privateKeyPath",    std::string{});
        d.publicKeyPath     = j.value("publicKeyPath",     std::string{});
        d.keepaliveSeconds  = j.value("keepaliveSeconds",  30);
        d.connectTimeoutSec = j.value("connectTimeoutSec", 10);
        d.remoteCommand     = j.value("remoteCommand",     std::string{});
        d.compress          = j.value("compress",          false);
        // password and passphrase are never stored — left at default ""
        return d;
    }
    if (type == "serial") {
        term::session::SerialDesc d;
        d.device      = j.value("device",      std::string{});
        d.baudRate    = j.value("baudRate",    115200u);
        d.dataBits    = j.value("dataBits",    static_cast<unsigned short>(8));
        d.stopBits    = StopBitsFromString(j.value("stopBits",    std::string{"1"}));
        d.parity      = ParityFromString(j.value("parity",      std::string{"none"}));
        d.flowControl = FlowFromString(j.value("flowControl", std::string{"none"}));
        d.dialScript  = j.value("dialScript",  std::string{});
        return d;
    }
    return term::session::LoopbackDesc{};
}

inline json SerialiseSessionInit(const term::session::SessionInit& si)
{
    json envVars = json::array();
    for (const auto& ev : si.envVars)
        envVars.push_back({{"key", ev.key}, {"value", ev.value}});

    return json{
        {"workingDir",   si.workingDir},
        {"loginShell",   si.loginShell},
        {"envFilePath",  si.envFilePath},
        {"envVars",      std::move(envVars)},
    };
}

inline term::session::SessionInit DeserialiseSessionInit(const json& j)
{
    term::session::SessionInit si;
    si.workingDir   = j.value("workingDir",  std::string{});
    si.loginShell   = j.value("loginShell",  false);
    si.envFilePath  = j.value("envFilePath", std::string{});
    for (const auto& ev : j.value("envVars", json::array()))
        si.envVars.push_back({ev.value("key", std::string{}),
                               ev.value("value", std::string{})});
    return si;
}

} // namespace term::db::serialisation
