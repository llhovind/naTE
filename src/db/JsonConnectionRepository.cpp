#include "db/JsonConnectionRepository.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace term::db {

namespace {

constexpr int kSchemaVersion = 1;

// ---- Serialise TransportDesc -----------------------------------------------

std::string AuthMethodToString(term::session::SshAuthMethod m)
{
    switch (m) {
        case term::session::SshAuthMethod::Password:   return "password";
        case term::session::SshAuthMethod::PrivateKey: return "privatekey";
        default:                                        return "agent";
    }
}

term::session::SshAuthMethod AuthMethodFromString(const std::string& s)
{
    if (s == "password")   return term::session::SshAuthMethod::Password;
    if (s == "privatekey") return term::session::SshAuthMethod::PrivateKey;
    return term::session::SshAuthMethod::Agent;
}

std::string ParityToString(term::session::SerialParity p)
{
    switch (p) {
        case term::session::SerialParity::Even: return "even";
        case term::session::SerialParity::Odd:  return "odd";
        default:                                return "none";
    }
}

term::session::SerialParity ParityFromString(const std::string& s)
{
    if (s == "even") return term::session::SerialParity::Even;
    if (s == "odd")  return term::session::SerialParity::Odd;
    return term::session::SerialParity::None;
}

std::string FlowToString(term::session::SerialFlowControl f)
{
    switch (f) {
        case term::session::SerialFlowControl::Hardware: return "hardware";
        case term::session::SerialFlowControl::Software: return "software";
        default:                                         return "none";
    }
}

term::session::SerialFlowControl FlowFromString(const std::string& s)
{
    if (s == "hardware") return term::session::SerialFlowControl::Hardware;
    if (s == "software") return term::session::SerialFlowControl::Software;
    return term::session::SerialFlowControl::None;
}

std::string StopBitsToString(term::session::SerialStopBits sb)
{
    return sb == term::session::SerialStopBits::Two ? "2" : "1";
}

term::session::SerialStopBits StopBitsFromString(const std::string& s)
{
    return s == "2" ? term::session::SerialStopBits::Two
                    : term::session::SerialStopBits::One;
}

json SerialiseTransport(const term::session::TransportDesc& transport)
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

term::session::TransportDesc DeserialiseTransport(const json& j)
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

// ---- Profile serialisation -------------------------------------------------

json SerialiseProfile(const ConnectionProfile& p)
{
    json j = SerialiseTransport(p.transport);
    j["id"]          = p.id;
    j["name"]        = p.name;
    j["wrapMode"]    = p.wrapMode;
    j["columnWidth"] = p.columnWidth;
    j["rows"]        = p.rows;
    j["createdAt"]   = static_cast<long long>(p.createdAt);
    j["lastUsed"]    = static_cast<long long>(p.lastUsed);
    return j;
}

ConnectionProfile DeserialiseProfile(const json& j)
{
    ConnectionProfile p;
    p.id          = j.value("id",          std::string{});
    p.name        = j.value("name",        std::string{});
    p.wrapMode    = j.value("wrapMode",    false);
    p.columnWidth = j.value("columnWidth", static_cast<unsigned short>(80));
    p.rows        = j.value("rows",        static_cast<unsigned short>(24));
    p.createdAt   = static_cast<std::time_t>(j.value("createdAt", 0LL));
    p.lastUsed    = static_cast<std::time_t>(j.value("lastUsed",  0LL));
    p.transport   = DeserialiseTransport(j);
    return p;
}

} // namespace

// ---- JsonConnectionRepository ----------------------------------------------

JsonConnectionRepository::JsonConnectionRepository(std::string path)
    : m_path(std::move(path))
{}

std::string JsonConnectionRepository::TmpPath() const
{
    return m_path + ".tmp";
}

std::vector<ConnectionProfile> JsonConnectionRepository::LoadAll()
{
    std::ifstream file(m_path);
    if (!file.is_open())
        return {};  // first run — not an error

    json root;
    try {
        file >> root;
    } catch (const json::exception& e) {
        std::cerr << "[ConnectionStore] JSON parse error in " << m_path
                  << ": " << e.what() << '\n';
        return {};
    }

    std::vector<ConnectionProfile> profiles;
    const auto& arr = root.value("connections", json::array());
    profiles.reserve(arr.size());
    for (const auto& j : arr) {
        try {
            profiles.push_back(DeserialiseProfile(j));
        } catch (const json::exception& e) {
            std::cerr << "[ConnectionStore] Skipping malformed profile: " << e.what() << '\n';
        }
    }
    return profiles;
}

void JsonConnectionRepository::Save(const ConnectionProfile& incoming)
{
    // Load current state so we can upsert
    std::vector<ConnectionProfile> profiles = LoadAll();

    auto it = std::find_if(profiles.begin(), profiles.end(),
                           [&](const ConnectionProfile& p){ return p.id == incoming.id; });
    if (it != profiles.end())
        *it = incoming;
    else
        profiles.push_back(incoming);

    json root;
    root["version"] = kSchemaVersion;
    root["connections"] = json::array();
    for (const auto& p : profiles)
        root["connections"].push_back(SerialiseProfile(p));

    // Atomic write: write to .tmp then rename
    const std::string tmp = TmpPath();
    {
        std::ofstream out(tmp);
        if (!out.is_open()) {
            std::cerr << "[ConnectionStore] Cannot write to " << tmp << '\n';
            return;
        }
        out << root.dump(2) << '\n';
    }
    if (std::rename(tmp.c_str(), m_path.c_str()) != 0)
        std::cerr << "[ConnectionStore] rename failed: " << tmp << " -> " << m_path << '\n';
}

void JsonConnectionRepository::Delete(const std::string& id)
{
    std::vector<ConnectionProfile> profiles = LoadAll();
    profiles.erase(
        std::remove_if(profiles.begin(), profiles.end(),
                       [&](const ConnectionProfile& p){ return p.id == id; }),
        profiles.end());

    json root;
    root["version"] = kSchemaVersion;
    root["connections"] = json::array();
    for (const auto& p : profiles)
        root["connections"].push_back(SerialiseProfile(p));

    const std::string tmp = TmpPath();
    {
        std::ofstream out(tmp);
        if (!out.is_open()) {
            std::cerr << "[ConnectionStore] Cannot write to " << tmp << '\n';
            return;
        }
        out << root.dump(2) << '\n';
    }
    std::rename(tmp.c_str(), m_path.c_str());
}

} // namespace term::db
