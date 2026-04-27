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
        }
    }

    return cfg;
}
