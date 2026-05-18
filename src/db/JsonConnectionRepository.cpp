#include "db/JsonConnectionRepository.h"
#include "db/ConnectionSerialisation.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>

using json = nlohmann::json;
using namespace term::db::serialisation;

namespace term::db {

namespace {

constexpr int kSchemaVersion = 1;

// ---- Profile serialisation -------------------------------------------------

json SerialiseProfile(const ConnectionProfile& p)
{
    json j = SerialiseTransport(p.transport);
    j["id"]              = p.id;
    j["name"]            = p.name;
    j["wrapMode"]        = p.wrapMode;
    j["columnWidth"]     = p.columnWidth;
    j["rows"]            = p.rows;
    j["createdAt"]       = static_cast<long long>(p.createdAt);
    j["lastUsed"]        = static_cast<long long>(p.lastUsed);
    j["sessionInit"]     = SerialiseSessionInit(p.sessionInit);
    j["profileTitle"]    = p.profileTitle;
    j["useProfileTitle"] = p.useProfileTitle;
    return j;
}

ConnectionProfile DeserialiseProfile(const json& j)
{
    ConnectionProfile p;
    p.id             = j.value("id",             std::string{});
    p.name           = j.value("name",           std::string{});
    p.wrapMode       = j.value("wrapMode",       false);
    p.columnWidth    = j.value("columnWidth",    static_cast<unsigned short>(80));
    p.rows           = j.value("rows",           static_cast<unsigned short>(24));
    p.createdAt      = static_cast<std::time_t>(j.value("createdAt", 0LL));
    p.lastUsed       = static_cast<std::time_t>(j.value("lastUsed",  0LL));
    p.profileTitle   = j.value("profileTitle",   std::string{});
    p.useProfileTitle= j.value("useProfileTitle",false);
    p.transport      = DeserialiseTransport(j);
    if (j.contains("sessionInit"))
        p.sessionInit = DeserialiseSessionInit(j.at("sessionInit"));
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
