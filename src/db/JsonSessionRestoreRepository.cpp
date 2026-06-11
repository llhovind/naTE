#include "db/JsonSessionRestoreRepository.h"
#include "db/ConnectionSerialisation.h"
#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>

using json = nlohmann::json;
using namespace term::db::serialisation;

namespace term::db {

namespace {

constexpr int kSchemaVersion = 1;

json SerialiseConnection(const term::session::Connection& c)
{
    json j = SerialiseTransport(c.transport);
    j["label"]          = c.label;
    j["wrapMode"]       = c.wrapMode;
    j["columnWidth"]    = c.columnWidth;
    j["rows"]           = c.rows;
    j["sessionInit"]    = SerialiseSessionInit(c.sessionInit);
    j["profileTitle"]   = c.profileTitle;
    j["useProfileTitle"]= c.useProfileTitle;
    return j;
}

term::session::Connection DeserialiseConnection(const json& j)
{
    term::session::Connection c;
    c.label          = j.value("label",          std::string{});
    c.wrapMode       = j.value("wrapMode",       false);
    c.columnWidth    = j.value("columnWidth",    static_cast<unsigned short>(0));
    c.rows           = j.value("rows",           static_cast<unsigned short>(0));
    c.profileTitle   = j.value("profileTitle",   std::string{});
    c.useProfileTitle= j.value("useProfileTitle",false);
    c.transport      = DeserialiseTransport(j);
    if (j.contains("sessionInit"))
        c.sessionInit = DeserialiseSessionInit(j.at("sessionInit"));
    return c;
}

json SerialiseTile(const term::session::RestoreTile& tile)
{
    json sessions = json::array();
    for (const auto& rs : tile.sessions) {
        json sj = SerialiseConnection(rs.conn);
        sj["scrollbackUuid"] = rs.scrollbackUuid;
        sessions.push_back(std::move(sj));
    }
    return json{{"activeTabIndex", tile.activeTabIndex}, {"sessions", std::move(sessions)}};
}

term::session::RestoreTile DeserialiseTile(const json& j)
{
    term::session::RestoreTile tile;
    tile.activeTabIndex = j.value("activeTabIndex", 0);
    for (const auto& sj : j.value("sessions", json::array())) {
        try {
            term::session::RestoreSession rs;
            rs.conn           = DeserialiseConnection(sj);
            rs.scrollbackUuid = sj.value("scrollbackUuid", std::string{});
            tile.sessions.push_back(std::move(rs));
        } catch (const json::exception& e) {
            std::cerr << "[SessionRestore] Skipping malformed session: " << e.what() << '\n';
        }
    }
    return tile;
}

json SerialiseWindow(const term::session::RestoreWindow& w)
{
    json tiles = json::array();
    for (const auto& t : w.tiles)
        tiles.push_back(SerialiseTile(t));
    return json{{"x", w.x}, {"y", w.y}, {"width", w.width}, {"height", w.height},
                {"tiles", std::move(tiles)}};
}

term::session::RestoreWindow DeserialiseWindow(const json& j)
{
    term::session::RestoreWindow w;
    w.x      = j.value("x",      0);
    w.y      = j.value("y",      0);
    w.width  = j.value("width",  1200);
    w.height = j.value("height", 700);
    for (const auto& tj : j.value("tiles", json::array())) {
        try {
            auto tile = DeserialiseTile(tj);
            if (!tile.sessions.empty())
                w.tiles.push_back(std::move(tile));
        } catch (const json::exception& e) {
            std::cerr << "[SessionRestore] Skipping malformed tile: " << e.what() << '\n';
        }
    }
    return w;
}

} // namespace

JsonSessionRestoreRepository::JsonSessionRestoreRepository(std::string path)
    : m_path(std::move(path))
{}

std::string JsonSessionRestoreRepository::TmpPath() const
{
    return m_path + ".tmp";
}

bool JsonSessionRestoreRepository::HasSnapshot() const
{
    std::ifstream file(m_path);
    if (!file.is_open())
        return false;
    try {
        json root;
        file >> root;
        for (const auto& wj : root.value("windows", json::array()))
            for (const auto& tj : wj.value("tiles", json::array()))
                if (!tj.value("sessions", json::array()).empty())
                    return true;
    } catch (...) {}
    return false;
}

term::session::RestoreState JsonSessionRestoreRepository::Load() const
{
    term::session::RestoreState state;
    std::ifstream file(m_path);
    if (!file.is_open())
        return state;

    json root;
    try {
        file >> root;
    } catch (const json::exception& e) {
        std::cerr << "[SessionRestore] JSON parse error in " << m_path
                  << ": " << e.what() << '\n';
        return state;
    }

    state.schemaVersion = root.value("version", kSchemaVersion);
    for (const auto& wj : root.value("windows", json::array())) {
        try {
            auto w = DeserialiseWindow(wj);
            if (!w.tiles.empty())
                state.windows.push_back(std::move(w));
        } catch (const json::exception& e) {
            std::cerr << "[SessionRestore] Skipping malformed window: " << e.what() << '\n';
        }
    }
    return state;
}

void JsonSessionRestoreRepository::Save(const term::session::RestoreState& state)
{
    // Treat an empty state as a delete to avoid leaving a stale file.
    if (state.windows.empty()) {
        Delete();
        return;
    }

    json root;
    root["version"] = kSchemaVersion;
    root["windows"] = json::array();
    for (const auto& w : state.windows)
        root["windows"].push_back(SerialiseWindow(w));

    const std::string tmp = TmpPath();
    {
        std::ofstream out(tmp);
        if (!out.is_open()) {
            std::cerr << "[SessionRestore] Cannot write to " << tmp << '\n';
            return;
        }
        out << root.dump(2) << '\n';
    }
    if (std::rename(tmp.c_str(), m_path.c_str()) != 0)
        std::cerr << "[SessionRestore] rename failed: " << tmp << " -> " << m_path << '\n';
}

void JsonSessionRestoreRepository::Delete()
{
    std::remove(m_path.c_str());
}

} // namespace term::db
