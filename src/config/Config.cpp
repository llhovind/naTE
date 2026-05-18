#include "config/Config.h"
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>

namespace {

std::string trim(std::string_view sv) {
    const auto first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(first, last - first + 1));
}

std::vector<GeometryPreset> parseGeometryPresets(const std::string& val) {
    std::vector<GeometryPreset> result;
    std::istringstream ss(val);
    std::string token;
    while (std::getline(ss, token, ',')) {
        const std::string t = trim(token);
        if (t.empty()) continue;
        const auto x = t.find('x');
        if (x == std::string::npos) continue;
        try {
            const int cols = std::stoi(t.substr(0, x));
            const int rows = std::stoi(t.substr(x + 1));
            if (cols > 0 && cols <= 65535 && rows > 0 && rows <= 65535)
                result.push_back({static_cast<unsigned short>(cols),
                                  static_cast<unsigned short>(rows)});
        } catch (...) {}
    }
    if (result.empty())
        result = {{80, 24}, {132, 24}};
    return result;
}

} // namespace

AppConfig AppConfig::load(const std::string& path) {
    AppConfig cfg;

    std::ifstream file(path);
    if (!file.is_open()) return cfg;

    std::string section;
    std::string line;

    while (std::getline(file, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;

        if (t[0] == '[') {
            const auto end = t.find(']');
            if (end != std::string::npos)
                section = trim(t.substr(1, end - 1));
            continue;
        }

        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));

        auto toInt = [](const std::string& s, int def) -> int {
            try { return std::stoi(s); } catch (...) { return def; }
        };

        if (section == "Panel") {
            if      (key == "Columns")      cfg.columns      = toInt(val, cfg.columns);
            else if (key == "Rows")         cfg.rows         = toInt(val, cfg.rows);
            else if (key == "FontSize")     cfg.fontSize     = toInt(val, cfg.fontSize);
            else if (key == "PtyLineWidth") cfg.ptyLineWidth = toInt(val, cfg.ptyLineWidth);
        } else if (section == "Terminal") {
            if (key == "GeometryPresets")   cfg.geometryPresets = parseGeometryPresets(val);
        } else if (section == "Colors") {
            auto clamp = [](int v) -> uint8_t {
                return static_cast<uint8_t>(std::max(0, std::min(255, v)));
            };
            if      (key == "TextR") cfg.textColour.r = clamp(toInt(val, cfg.textColour.r));
            else if (key == "TextG") cfg.textColour.g = clamp(toInt(val, cfg.textColour.g));
            else if (key == "TextB") cfg.textColour.b = clamp(toInt(val, cfg.textColour.b));
            else if (key == "BgR")   cfg.bgColour.r   = clamp(toInt(val, cfg.bgColour.r));
            else if (key == "BgG")   cfg.bgColour.g   = clamp(toInt(val, cfg.bgColour.g));
            else if (key == "BgB")   cfg.bgColour.b   = clamp(toInt(val, cfg.bgColour.b));
        } else if (section == "Session") {
            if      (key == "DefaultWorkingDir") cfg.defaultWorkingDir  = val;
            else if (key == "DefaultEnvFile")    cfg.defaultEnvFilePath = val;
            else if (key == "DefaultLoginShell")    cfg.defaultLoginShell   = (val == "true" || val == "1");
            else if (key == "AutoRestoreSession")   cfg.autoRestoreSession  = (val == "true" || val == "1");
            else if (key == "SessionSaveInterval")  cfg.sessionSaveInterval = toInt(val, cfg.sessionSaveInterval);
            else {
                // Indexed env var pairs: EnvVar0Key / EnvVar0Value, EnvVar1Key / EnvVar1Value, …
                // Stops at the first index with no Key entry; max 64 pairs.
                constexpr int kMaxEnvVars = 64;
                for (int i = 0; i < kMaxEnvVars; ++i) {
                    const std::string keyKey = "EnvVar" + std::to_string(i) + "Key";
                    const std::string valKey = "EnvVar" + std::to_string(i) + "Value";
                    if (key == keyKey) {
                        if (static_cast<int>(cfg.defaultEnvVars.size()) <= i)
                            cfg.defaultEnvVars.resize(i + 1);
                        cfg.defaultEnvVars[i].key = val;
                        break;
                    } else if (key == valKey) {
                        if (static_cast<int>(cfg.defaultEnvVars.size()) <= i)
                            cfg.defaultEnvVars.resize(i + 1);
                        cfg.defaultEnvVars[i].value = val;
                        break;
                    }
                }
            }
        }
    }

    // Remove any env var entries where the key is empty (incomplete pairs).
    cfg.defaultEnvVars.erase(
        std::remove_if(cfg.defaultEnvVars.begin(), cfg.defaultEnvVars.end(),
                       [](const term::session::EnvVar& ev) { return ev.key.empty(); }),
        cfg.defaultEnvVars.end());

    return cfg;
}
