#pragma once
// Internal header: shared serialisation helpers for Connection transport and
// SessionInit fields. Included by JsonConnectionRepository.cpp and
// JsonSessionRestoreRepository.cpp — not part of the public API.

#include "session/Connection.h"
#include <nlohmann/json.hpp>
#include <string>

namespace term::db::serialisation {

using json = nlohmann::json;

inline std::string AuthMethodToString(term::transport::SshAuthMethod m)
{
    switch (m) {
        case term::transport::SshAuthMethod::Password:       return "password";
        case term::transport::SshAuthMethod::PrivateKey:     return "privatekey";
        case term::transport::SshAuthMethod::KbdInteractive: return "keyboard-interactive";
        default:                                            return "agent";
    }
}

inline term::transport::SshAuthMethod AuthMethodFromString(const std::string& s)
{
    if (s == "password")             return term::transport::SshAuthMethod::Password;
    if (s == "privatekey")           return term::transport::SshAuthMethod::PrivateKey;
    if (s == "keyboard-interactive") return term::transport::SshAuthMethod::KbdInteractive;
    return term::transport::SshAuthMethod::Agent;
}

inline std::string ParityToString(term::transport::SerialParity p)
{
    switch (p) {
        case term::transport::SerialParity::Even: return "even";
        case term::transport::SerialParity::Odd:  return "odd";
        default:                                return "none";
    }
}

inline term::transport::SerialParity ParityFromString(const std::string& s)
{
    if (s == "even") return term::transport::SerialParity::Even;
    if (s == "odd")  return term::transport::SerialParity::Odd;
    return term::transport::SerialParity::None;
}

inline std::string FlowToString(term::transport::SerialFlowControl f)
{
    switch (f) {
        case term::transport::SerialFlowControl::Hardware: return "hardware";
        case term::transport::SerialFlowControl::Software: return "software";
        default:                                         return "none";
    }
}

inline term::transport::SerialFlowControl FlowFromString(const std::string& s)
{
    if (s == "hardware") return term::transport::SerialFlowControl::Hardware;
    if (s == "software") return term::transport::SerialFlowControl::Software;
    return term::transport::SerialFlowControl::None;
}

inline std::string StopBitsToString(term::transport::SerialStopBits sb)
{
    return sb == term::transport::SerialStopBits::Two ? "2" : "1";
}

inline term::transport::SerialStopBits StopBitsFromString(const std::string& s)
{
    return s == "2" ? term::transport::SerialStopBits::Two
                    : term::transport::SerialStopBits::One;
}

inline json SerialiseTransport(const term::transport::TransportDesc& transport)
{
    return std::visit([](const auto& desc) -> json {
        using T = std::decay_t<decltype(desc)>;
        if constexpr (std::is_same_v<T, term::transport::PtyDesc>) {
            return json{{"type", "pty"}, {"shell", desc.shell}, {"command", desc.command}};
        } else if constexpr (std::is_same_v<T, term::transport::SshDesc>) {
            // passwords and passphrases are intentionally excluded
            json j{
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
                {"compress",             desc.compress},
                {"x11Forwarding",        desc.x11Forwarding},
                {"agentForwarding",      desc.agentForwarding},
                {"agentIdentityHint",    desc.agentIdentityHint},
            };
            if (desc.proxyJump && !desc.proxyJump->host.empty()) {
                j["proxyJump"] = {
                    {"host",              desc.proxyJump->host},
                    {"port",              desc.proxyJump->port},
                    {"user",             desc.proxyJump->user},
                    {"authMethod",        AuthMethodToString(desc.proxyJump->authMethod)},
                    {"privateKeyPath",    desc.proxyJump->privateKeyPath},
                    {"publicKeyPath",     desc.proxyJump->publicKeyPath},
                    {"agentIdentityHint", desc.proxyJump->agentIdentityHint},
                };
            }
            if (!desc.portForwards.empty()) {
                json pfArr = json::array();
                for (const auto& pf : desc.portForwards) {
                    // id is runtime-only; not persisted
                    pfArr.push_back({
                        {"direction", pf.direction == term::transport::PortForwardDirection::Remote
                                       ? "remote" : "local"},
                        {"bindAddr",   pf.bindAddr},
                        {"localPort",  pf.localPort},
                        {"remoteHost", pf.remoteHost},
                        {"remotePort", pf.remotePort},
                        {"label",      pf.label},
                    });
                }
                j["portForwards"] = std::move(pfArr);
            }
            return j;
        } else if constexpr (std::is_same_v<T, term::transport::SerialDesc>) {
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

inline term::transport::TransportDesc DeserialiseTransport(const json& j)
{
    const std::string type = j.value("type", "loopback");

    if (type == "pty") {
        return term::transport::PtyDesc{
            j.value("shell",   std::string{}),
            j.value("command", std::string{}),
        };
    }
    if (type == "ssh") {
        term::transport::SshDesc d;
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
        d.x11Forwarding     = j.value("x11Forwarding",     false);
        d.agentForwarding   = j.value("agentForwarding",   false);
        d.agentIdentityHint = j.value("agentIdentityHint", std::string{});
        // password and passphrase are never stored — left at default ""
        if (j.contains("portForwards")) {
            term::transport::PortForwardId nextId = 1;
            for (const auto& pf : j["portForwards"]) {
                term::transport::PortForwardDesc pfDesc;
                pfDesc.id         = nextId++;  // assign sequential runtime IDs
                pfDesc.direction  = pf.value("direction", std::string{"local"}) == "remote"
                                    ? term::transport::PortForwardDirection::Remote
                                    : term::transport::PortForwardDirection::Local;
                pfDesc.bindAddr   = pf.value("bindAddr",   std::string{"127.0.0.1"});
                pfDesc.localPort  = pf.value("localPort",  static_cast<uint16_t>(0));
                pfDesc.remoteHost = pf.value("remoteHost", std::string{"localhost"});
                pfDesc.remotePort = pf.value("remotePort", static_cast<uint16_t>(0));
                pfDesc.label      = pf.value("label",      std::string{});
                d.portForwards.push_back(std::move(pfDesc));
            }
        }
        if (j.contains("proxyJump")) {
            const auto& pj = j["proxyJump"];
            const std::string pjHost = pj.value("host", std::string{});
            if (!pjHost.empty()) {
                term::transport::ProxyJumpDesc jump;
                jump.host              = pjHost;
                jump.port              = pj.value("port",              static_cast<unsigned short>(22));
                jump.user              = pj.value("user",              std::string{});
                jump.authMethod        = AuthMethodFromString(pj.value("authMethod", std::string{"agent"}));
                jump.privateKeyPath    = pj.value("privateKeyPath",    std::string{});
                jump.publicKeyPath     = pj.value("publicKeyPath",     std::string{});
                jump.agentIdentityHint = pj.value("agentIdentityHint", std::string{});
                d.proxyJump = std::move(jump);
            }
        }
        return d;
    }
    if (type == "serial") {
        term::transport::SerialDesc d;
        d.device      = j.value("device",      std::string{});
        d.baudRate    = j.value("baudRate",    115200u);
        d.dataBits    = j.value("dataBits",    static_cast<unsigned short>(8));
        d.stopBits    = StopBitsFromString(j.value("stopBits",    std::string{"1"}));
        d.parity      = ParityFromString(j.value("parity",      std::string{"none"}));
        d.flowControl = FlowFromString(j.value("flowControl", std::string{"none"}));
        d.dialScript  = j.value("dialScript",  std::string{});
        return d;
    }
    return term::transport::LoopbackDesc{};
}

inline json SerialiseSessionInit(const term::transport::SessionInit& si)
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

inline term::transport::SessionInit DeserialiseSessionInit(const json& j)
{
    term::transport::SessionInit si;
    si.workingDir   = j.value("workingDir",  std::string{});
    si.loginShell   = j.value("loginShell",  false);
    si.envFilePath  = j.value("envFilePath", std::string{});
    for (const auto& ev : j.value("envVars", json::array()))
        si.envVars.push_back({ev.value("key", std::string{}),
                               ev.value("value", std::string{})});
    return si;
}

} // namespace term::db::serialisation
