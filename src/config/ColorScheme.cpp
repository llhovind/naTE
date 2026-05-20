#include "config/ColorScheme.h"
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <fstream>
#include <iomanip>
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

// Parse "r,g,b" into Rgb. Returns nullopt if the string is malformed.
std::optional<Rgb> parseRgb(const std::string& s)
{
    std::istringstream ss(s);
    int r = 0, g = 0, b = 0;
    char comma1 = 0, comma2 = 0;
    if (!(ss >> r >> comma1 >> g >> comma2 >> b)) return std::nullopt;
    if (comma1 != ',' || comma2 != ',')           return std::nullopt;
    auto clamp = [](int v) { return static_cast<uint8_t>(std::max(0, std::min(255, v))); };
    return Rgb{ clamp(r), clamp(g), clamp(b) };
}

// Parse a 6-digit hex string "rrggbb" (no leading #) into Rgb.
std::optional<Rgb> parseHex(const std::string& s)
{
    if (s.size() != 6) return std::nullopt;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    unsigned int v = 0;
    std::istringstream ss(s);
    ss >> std::hex >> v;
    return Rgb{
        static_cast<uint8_t>((v >> 16) & 0xFF),
        static_cast<uint8_t>((v >>  8) & 0xFF),
        static_cast<uint8_t>( v        & 0xFF)
    };
}

// Map a base16 key name ("base00"-"base0F") to a palette index (0-15).
// Returns -1 if the key is not a recognised base16 name.
int base16Index(const std::string& key)
{
    if (key.size() != 6 || key.substr(0, 4) != "base") return -1;
    const std::string hex = key.substr(4);
    if (hex.size() != 2) return -1;
    // Accept both upper and lower case suffix (base0A or base0a).
    std::string h = hex;
    h[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(h[0])));
    h[1] = static_cast<char>(std::toupper(static_cast<unsigned char>(h[1])));
    const std::string valid = "0123456789ABCDEF";
    const auto i = valid.find(h[0]);
    const auto j = valid.find(h[1]);
    if (i == std::string::npos || j == std::string::npos) return -1;
    const int idx = static_cast<int>(i) * 16 + static_cast<int>(j);
    return (idx < 16) ? idx : -1;
}

// Extract the filename stem (basename without extension).
std::string stemFromPath(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const auto dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

// Canonical base16-to-ANSI index mapping (base16 styling guidelines v0.2).
// ansiColors[i] = palette[kAnsiMap[i]].
constexpr std::array<int, 16> kAnsiMap = {
     0,   // ANSI  0 black         = base00
     8,   // ANSI  1 red           = base08
    11,   // ANSI  2 green         = base0B
    10,   // ANSI  3 yellow        = base0A
    13,   // ANSI  4 blue          = base0D
    14,   // ANSI  5 magenta       = base0E
    12,   // ANSI  6 cyan          = base0C
     5,   // ANSI  7 white         = base05
     3,   // ANSI  8 bright black  = base03
     8,   // ANSI  9 bright red    = base08
    11,   // ANSI 10 bright green  = base0B
    10,   // ANSI 11 bright yellow = base0A
    13,   // ANSI 12 bright blue   = base0D
    14,   // ANSI 13 bright magenta= base0E
    12,   // ANSI 14 bright cyan   = base0C
     7,   // ANSI 15 bright white  = base07
};

} // namespace

void ColorScheme::computeAnsiColors()
{
    for (int i = 0; i < 16; ++i)
        ansiColors[i] = palette[kAnsiMap[i]];
}

std::optional<ColorScheme> ColorScheme::loadFromFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    ColorScheme scheme;
    scheme.stem = stemFromPath(path);
    scheme.displayName = scheme.stem;  // fallback if [Meta] Name is absent

    std::string section;
    std::string line;
    bool hasFg = false, hasBg = false, hasCursor = false;

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
        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));

        if (section == "Meta") {
            if (key == "Name" && !val.empty()) scheme.displayName = val;
        } else if (section == "Colors") {
            if (key == "Foreground") {
                if (auto rgb = parseRgb(val)) { scheme.foreground = *rgb; hasFg = true; }
            } else if (key == "Background") {
                if (auto rgb = parseRgb(val)) { scheme.background = *rgb; hasBg = true; }
            } else if (key == "Cursor") {
                if (auto rgb = parseHex(val)) { scheme.cursor = *rgb; hasCursor = true; }
            }
        } else if (section == "Palette") {
            const int idx = base16Index(key);
            if (idx >= 0) {
                if (auto rgb = parseHex(val)) {
                    scheme.palette[idx] = *rgb;
                    scheme.hasPalette = true;
                }
            }
        } else if (section == "ANSI") {
            // Direct ANSI palette: keys are "0"–"15", values are 6-digit hex.
            try {
                const int idx = std::stoi(key);
                if (idx >= 0 && idx < 16) {
                    if (auto rgb = parseHex(val)) {
                        scheme.ansiColors[idx] = *rgb;
                        scheme.hasDirectAnsi   = true;
                    }
                }
            } catch (...) {}
        }
    }

    // Resolve ANSI palette and default fg/bg/cursor.
    if (scheme.hasDirectAnsi) {
        // [ANSI] section wins: ansiColors already populated; mark palette present
        // so Config picks it up, but skip the base16 mapping step.
        scheme.hasPalette = true;
        if (!hasCursor) scheme.cursor = scheme.foreground;
    } else if (scheme.hasPalette) {
        // [Palette] (base16): derive ANSI colours and default fg/bg from base05/base00.
        scheme.computeAnsiColors();
        if (!hasBg)     scheme.background = scheme.palette[0];   // base00
        if (!hasFg)     scheme.foreground = scheme.palette[5];   // base05
        if (!hasCursor) scheme.cursor     = scheme.palette[5];   // base05 convention
        hasFg = hasBg = true;
    } else if (!hasCursor) {
        scheme.cursor = scheme.foreground;
    }

    // A theme file must supply at least one colour to be considered valid.
    if (!hasFg && !hasBg) return std::nullopt;
    return scheme;
}

void ColorScheme::saveToFile(const std::string& path) const
{
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return;

    f << "[Meta]\n"
      << "Name=" << displayName << "\n"
      << "\n"
      << "[Colors]\n"
      << "Foreground=" << static_cast<int>(foreground.r) << ","
                       << static_cast<int>(foreground.g) << ","
                       << static_cast<int>(foreground.b) << "\n"
      << "Background=" << static_cast<int>(background.r) << ","
                       << static_cast<int>(background.g) << ","
                       << static_cast<int>(background.b) << "\n";

    if (hasDirectAnsi) {
        f << "\n[ANSI]\n";
        for (int i = 0; i < 16; ++i) {
            f << i << "="
              << std::hex << std::setfill('0')
              << std::setw(2) << static_cast<int>(ansiColors[i].r)
              << std::setw(2) << static_cast<int>(ansiColors[i].g)
              << std::setw(2) << static_cast<int>(ansiColors[i].b)
              << std::dec << "\n";
        }
    } else if (hasPalette) {
        f << "\n[Palette]\n";
        static const char* kBase16Names[16] = {
            "base00","base01","base02","base03",
            "base04","base05","base06","base07",
            "base08","base09","base0A","base0B",
            "base0C","base0D","base0E","base0F",
        };
        for (int i = 0; i < 16; ++i) {
            f << kBase16Names[i] << "="
              << std::hex << std::setfill('0')
              << std::setw(2) << static_cast<int>(palette[i].r)
              << std::setw(2) << static_cast<int>(palette[i].g)
              << std::setw(2) << static_cast<int>(palette[i].b)
              << std::dec << "\n";
        }
    }
}

std::vector<ColorScheme> ColorScheme::scanDirectory(const std::string& dir)
{
    std::vector<ColorScheme> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;

    while (dirent* entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".ini") continue;
        if (auto scheme = loadFromFile(dir + "/" + name))
            result.push_back(std::move(*scheme));
    }
    closedir(d);

    std::sort(result.begin(), result.end(), [](const ColorScheme& a, const ColorScheme& b) {
        return a.displayName < b.displayName;
    });
    return result;
}
