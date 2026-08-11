#include "config/Config.h"
#include "config/ColorScheme.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string trim(std::string_view sv)
{
    const auto first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(first, last - first + 1));
}

std::vector<GeometryPreset> parseGeometryPresets(const std::string& val)
{
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
    if (result.empty()) result = {{80, 24}, {132, 24}};
    return result;
}

// Map legacy enum names written by earlier versions to the new file stems.
std::string normalizeLegacyThemeName(const std::string& val)
{
    if (val == "SolarizedDark")  return "solarized-dark";
    if (val == "SolarizedLight") return "solarized-light";
    if (val == "Custom")         return "";   // no file; keep fallback colours
    return val;
}

} // namespace

AppConfig AppConfig::load(const std::string& configPath, const std::string& themesDir)
{
    AppConfig cfg;

    std::ifstream file(configPath);
    if (!file.is_open()) return cfg;

    struct KV { std::string section, key, val; };
    std::vector<KV> entries;

    std::string section;
    std::string line;
    while (std::getline(file, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t[0] == '[') {
            const auto end = t.find(']');
            if (end != std::string::npos) section = trim(t.substr(1, end - 1));
            continue;
        }
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        entries.push_back({ section, trim(t.substr(0, eq)), trim(t.substr(eq + 1)) });
    }

    auto toInt = [](const std::string& s, int def) -> int {
        try { return std::stoi(s); } catch (...) { return def; }
    };

    for (const auto& e : entries) {
        const auto& sec = e.section;
        const auto& key = e.key;
        const auto& val = e.val;

        if (sec == "Panel") {
            if      (key == "Columns")         cfg.columns         = toInt(val, cfg.columns);
            else if (key == "Rows")            cfg.rows            = toInt(val, cfg.rows);
            else if (key == "FontSize")        cfg.fontSize        = toInt(val, cfg.fontSize);
            else if (key == "PtyLineWidth")    cfg.ptyLineWidth    = toInt(val, cfg.ptyLineWidth);
            else if (key == "ScrollbackLines") cfg.scrollbackLines = toInt(val, cfg.scrollbackLines);
        } else if (sec == "Appearance") {
            if      (key == "ColorTheme")  cfg.themeName  = normalizeLegacyThemeName(val);
            else if (key == "FontFamily")  cfg.fontFamily = val;
            else if (key == "Padding")     cfg.padding    = toInt(val, cfg.padding);
            else if (key == "CursorStyle") {
                if      (val == "Bar")       cfg.cursorStyle = CursorStyle::Bar;
                else if (val == "Underline") cfg.cursorStyle = CursorStyle::Underline;
                else                         cfg.cursorStyle = CursorStyle::Block;
            }
            else if (key == "CursorBlink") cfg.cursorBlink = (val == "true" || val == "1");
            else if (key == "TileLayout") {
                if (val == "ColumnFirst") cfg.tileLayout = TileLayout::ColumnFirst;
                else                      cfg.tileLayout = TileLayout::RowFirst;
            }
        } else if (sec == "Behavior") {
            if      (key == "Encoding"         && !val.empty()) cfg.encoding        = val;
            else if (key == "WebSearchUrl"     && !val.empty()) cfg.webSearchUrl    = val;
            else if (key == "WordSelectRegex"  && !val.empty()) cfg.wordSelectRegex = val;
            else if (key == "CopyOnSelect")          cfg.copyOnSelect          = (val == "true" || val == "1");
            else if (key == "ConfirmCloseWindow")    cfg.confirmCloseWindow    = (val == "true" || val == "1");
            else if (key == "FileExplorerWidth")     cfg.fileExplorerWidth     = std::stoi(val);
            else if (key == "FileExplorerHeight")    cfg.fileExplorerHeight    = std::stoi(val);
            // RemoteEditorCommand is the pre-local-edit spelling. Still read so
            // an existing setting survives the upgrade; only the new key is
            // written, so it disappears on the next save.
            else if (key == "ExternalEditorCommand" ||
                     key == "RemoteEditorCommand")   cfg.externalEditorCommand = val;
            else if (key == "BellMode") {
                if      (val == "None")    cfg.bellMode = BellMode::None;
                else if (val == "Audible") cfg.bellMode = BellMode::Audible;
                else                       cfg.bellMode = BellMode::Visual;
            }
        } else if (sec == "Terminal") {
            if (key == "GeometryPresets") cfg.geometryPresets = parseGeometryPresets(val);
        } else if (sec == "Session") {
            if      (key == "DefaultShell")        cfg.defaultShell       = val;
            else if (key == "DefaultWorkingDir")   cfg.defaultWorkingDir  = val;
            else if (key == "DefaultEnvFile")      cfg.defaultEnvFilePath = val;
            else if (key == "DefaultLoginShell")   cfg.defaultLoginShell  = (val == "true" || val == "1");
            else if (key == "DefaultWrapMode")     cfg.defaultWrapMode    = (val == "true" || val == "1");
            else if (key == "AutoRestoreSession")  cfg.autoRestoreSession  = (val == "true" || val == "1");
            else if (key == "SessionSaveInterval") cfg.sessionSaveInterval = toInt(val, cfg.sessionSaveInterval);
            else if (key == "SaveScrollbackWithWorkspace") cfg.saveScrollbackWithWorkspace = (val == "true" || val == "1");
            else if (key == "ScrollbackSaveLines")         cfg.scrollbackSaveLines = toInt(val, cfg.scrollbackSaveLines);
            else if (key == "ScrollbackSaveStyles")        cfg.scrollbackSaveStyles = (val == "true" || val == "1");
            else {
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

    cfg.defaultEnvVars.erase(
        std::remove_if(cfg.defaultEnvVars.begin(), cfg.defaultEnvVars.end(),
                       [](const term::transport::EnvVar& ev) { return ev.key.empty(); }),
        cfg.defaultEnvVars.end());

    // Resolve theme colours from file; keep struct defaults if unavailable.
    if (!cfg.themeName.empty() && !themesDir.empty()) {
        const std::string themePath = themesDir + "/" + cfg.themeName + ".ini";
        if (auto scheme = ColorScheme::loadFromFile(themePath)) {
            cfg.textColour   = scheme->foreground;
            cfg.bgColour     = scheme->background;
            cfg.cursorColour = scheme->cursor;
            if (scheme->hasPalette)
                cfg.ansiColors = scheme->ansiColors;
            cfg.uiColors = scheme->deriveUiColors();
        }
    }

    return cfg;
}

void AppConfig::save(const std::string& configPath) const
{
    std::ofstream f(configPath, std::ios::trunc);
    if (!f.is_open()) return;

    const char* tileLayoutStr = (tileLayout == TileLayout::ColumnFirst) ? "ColumnFirst" : "RowFirst";
    const char* cursorStyleStr = [&]() -> const char* {
        switch (cursorStyle) {
            case CursorStyle::Bar:       return "Bar";
            case CursorStyle::Underline: return "Underline";
            default:                     return "Block";
        }
    }();
    const char* bellModeStr = [&]() -> const char* {
        switch (bellMode) {
            case BellMode::None:    return "None";
            case BellMode::Audible: return "Audible";
            default:                return "Visual";
        }
    }();

    f << "[Appearance]\n"
      << "ColorTheme="  << themeName                        << "\n"
      << "FontFamily="  << fontFamily                       << "\n"
      << "Padding="     << padding                          << "\n"
      << "CursorStyle=" << cursorStyleStr                   << "\n"
      << "CursorBlink=" << (cursorBlink ? "true" : "false") << "\n"
      << "TileLayout="  << tileLayoutStr                    << "\n"
      << "\n"
      << "[Behavior]\n"
      << "Encoding="        << encoding                          << "\n"
      << "WebSearchUrl="    << webSearchUrl                      << "\n"
      << "WordSelectRegex=" << wordSelectRegex                   << "\n"
      << "BellMode="        << bellModeStr                       << "\n"
      << "CopyOnSelect="          << (copyOnSelect       ? "true" : "false") << "\n"
      << "ConfirmCloseWindow="    << (confirmCloseWindow ? "true" : "false") << "\n"
      << "FileExplorerWidth="     << fileExplorerWidth                        << "\n"
      << "FileExplorerHeight="    << fileExplorerHeight                       << "\n"
      << "ExternalEditorCommand=" << externalEditorCommand                    << "\n"
      << "\n"
      << "[Panel]\n"
      << "Columns="         << columns         << "\n"
      << "Rows="            << rows            << "\n"
      << "FontSize="        << fontSize        << "\n"
      << "PtyLineWidth="    << ptyLineWidth    << "\n"
      << "ScrollbackLines=" << scrollbackLines << "\n"
      << "\n"
      << "[Terminal]\n"
      << "GeometryPresets=";

    for (std::size_t i = 0; i < geometryPresets.size(); ++i) {
        if (i) f << ",";
        f << geometryPresets[i].cols << "x" << geometryPresets[i].rows;
    }

    f << "\n\n"
      << "[Session]\n"
      << "DefaultShell="        << defaultShell       << "\n"
      << "DefaultWorkingDir="   << defaultWorkingDir  << "\n"
      << "DefaultEnvFile="      << defaultEnvFilePath << "\n"
      << "DefaultLoginShell="   << (defaultLoginShell  ? "true" : "false") << "\n"
      << "DefaultWrapMode="     << (defaultWrapMode    ? "true" : "false") << "\n"
      << "AutoRestoreSession="  << (autoRestoreSession ? "true" : "false") << "\n"
      << "SessionSaveInterval=" << sessionSaveInterval << "\n"
      << "SaveScrollbackWithWorkspace=" << (saveScrollbackWithWorkspace ? "true" : "false") << "\n"
      << "ScrollbackSaveLines="         << scrollbackSaveLines << "\n"
      << "ScrollbackSaveStyles="        << (scrollbackSaveStyles ? "true" : "false") << "\n";

    for (std::size_t i = 0; i < defaultEnvVars.size(); ++i) {
        f << "EnvVar" << i << "Key="   << defaultEnvVars[i].key   << "\n"
          << "EnvVar" << i << "Value=" << defaultEnvVars[i].value << "\n";
    }
}
