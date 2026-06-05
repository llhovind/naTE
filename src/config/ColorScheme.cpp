#include "config/ColorScheme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
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
                    scheme.palette[idx]        = *rgb;
                    scheme.hasPalette          = true;
                    scheme.hasPaletteSection   = true;
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

// ---------------------------------------------------------------------------
// UI chrome color derivation
// ---------------------------------------------------------------------------

namespace {

// Linearly blend a toward b by t (0.0 = all a, 1.0 = all b).
Rgb blendRgb(Rgb a, Rgb b, double t) {
    auto ch = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::round(x * (1.0 - t) + y * t));
    };
    return { ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b) };
}

// Choose `light` or `dark` based on the perceived luminance of `bg`.
// Threshold 140 works well across Solarized Dark/Light and most base16 themes.
Rgb contrastOn(Rgb bg, Rgb light, Rgb dark) {
    const double lum = 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b;
    return lum > 140.0 ? dark : light;
}

} // namespace

UiColors ColorScheme::deriveUiColors() const
{
    if (hasPaletteSection) {
        // Full base16 palette: rich semantic mapping.
        // palette[N] = baseN (base00=0 … base0F=15).
        const auto& p = palette;

        UiColors u;
        u.frameBackground   = p[1];   // base01 — slightly lighter chrome background
        u.tileActive        = p[13];  // base0D — theme accent (blue in most dark themes)
        u.tileInactive      = p[3];   // base03 — muted / comments color
        u.tileBroadcast     = p[9];   // base09 — orange / warm accent

        // contrastOn picks light or dark text automatically — keeps light themes legible.
        u.tabText           = contrastOn(p[3], p[7], p[0]);  // base07 or base00
        u.tabCloseActive    = p[6];   // base06 — slightly dimmer than tabText
        u.tabCloseInactive  = p[3];   // base03 — same as inactive bg (appears dim)

        u.statusUnread       = p[12]; // base0C — cyan
        u.statusDisconnected = p[8];  // base08 — red
        u.statusReconnecting = p[10]; // base0A — yellow

        u.reconnectBarBg    = p[9];   // base09 — warm orange "warning"
        u.reconnectBarText  = contrastOn(p[9], p[7], p[0]);

        // 20% accent tint over terminal background — subtle "active overlay".
        u.searchBarBg       = blendRgb(p[0], p[13], 0.20);

        u.controlActive     = contrastOn(p[3], p[7], p[0]);

        u.selectionBg     = p[13];  // base0D — accent blue, high contrast against terminal bg
        u.selectionFg     = contrastOn(p[13], p[7], p[0]);
        u.searchMatchBg   = p[10];  // base0A — yellow/warning
        u.searchMatchFg   = contrastOn(p[10], p[7], p[0]);
        u.searchCurrentBg = p[9];   // base09 — orange/active

        return u;
    }

    if (hasDirectAnsi) {
        // ANSI-only theme: no base16 semantic slots, but we can map standard
        // ANSI roles (blue=accent, bright-black=inactive, cyan/red/yellow=status).
        // ANSI indices: 0=black 1=red 2=green 3=yellow 4=blue 5=magenta 6=cyan
        //               7=white 8=bright-black 9=bright-red 10=bright-green
        //              11=bright-yellow 12=bright-blue 13=bright-magenta
        //              14=bright-cyan 15=bright-white
        const auto& a = ansiColors;

        UiColors u;
        u.frameBackground    = blendRgb(a[0], a[8], 0.25);  // bg + slight gray
        u.tileActive         = a[4];    // ANSI blue — standard "active" accent
        u.tileInactive       = a[8];    // ANSI bright-black (dark gray)
        u.tileBroadcast      = a[3];    // ANSI yellow — warm accent

        u.tabText            = contrastOn(a[4], a[15], a[0]);
        u.tabCloseActive     = a[7];    // ANSI white
        u.tabCloseInactive   = a[8];    // ANSI bright-black

        u.statusUnread       = a[6];    // ANSI cyan
        u.statusDisconnected = a[1];    // ANSI red
        u.statusReconnecting = a[3];    // ANSI yellow

        u.reconnectBarBg     = a[1];    // ANSI red — "something is wrong"
        u.reconnectBarText   = contrastOn(a[1], a[15], a[0]);

        u.searchBarBg        = blendRgb(a[0], a[4], 0.20);  // slight blue tint on bg

        u.controlActive      = a[15];   // ANSI bright-white

        u.selectionBg     = a[12];   // ANSI bright-blue — visible accent
        u.selectionFg     = contrastOn(a[12], a[15], a[0]);
        u.searchMatchBg   = a[3];    // ANSI yellow
        u.searchMatchFg   = contrastOn(a[3], a[15], a[0]);
        u.searchCurrentBg = a[11];   // ANSI bright-yellow

        return u;
    }

    // No palette at all ([Colors]-only theme) — use Solarized Dark defaults.
    return UiColors{};
}

// ---------------------------------------------------------------------------
// YAML loader — supports base16 v0.x (flat) and tinted-theming v2 (palette: block)
// ---------------------------------------------------------------------------

std::optional<ColorScheme> ColorScheme::loadFromYaml(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    ColorScheme scheme;
    scheme.stem        = stemFromPath(path);
    scheme.displayName = scheme.stem;

    std::string line;
    while (std::getline(file, line)) {
        // Strip leading whitespace so indented keys (palette: block) parse identically
        // to top-level keys (flat format).
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;

        const auto colon = t.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(t.substr(0, colon));
        std::string val = trim(t.substr(colon + 1));

        // Extract the value token, stripping quotes and inline comments (# ...).
        // For quoted values find the closing quote; for unquoted take up to the
        // first whitespace so " base00: f8f8f2 # comment" works correctly.
        if (!val.empty() && (val[0] == '"' || val[0] == '\'')) {
            const char q = val[0];
            const auto close = val.find(q, 1);
            val = (close != std::string::npos) ? val.substr(1, close - 1) : val.substr(1);
        } else {
            const auto sp = val.find_first_of(" \t");
            if (sp != std::string::npos) val = val.substr(0, sp);
        }

        // Strip optional leading '#' from hex values (e.g. "#f8f8f2").
        if (!val.empty() && val[0] == '#') val = val.substr(1);

        if (val.empty()) continue;  // e.g. the "palette:" key in v2 format

        // Display name: "scheme" (v0.x) or "name" (v2).
        if (key == "scheme" || key == "name") {
            scheme.displayName = val;
            continue;
        }

        // Palette slot: base00–base0F.
        const int idx = base16Index(key);
        if (idx >= 0) {
            if (auto rgb = parseHex(val)) {
                scheme.palette[idx]      = *rgb;
                scheme.hasPalette        = true;
                scheme.hasPaletteSection = true;
            }
        }
    }

    if (!scheme.hasPalette) return std::nullopt;

    scheme.computeAnsiColors();
    scheme.background = scheme.palette[0];  // base00
    scheme.foreground = scheme.palette[5];  // base05
    scheme.cursor     = scheme.palette[5];  // base05 convention

    return scheme;
}

// ---------------------------------------------------------------------------
// Directory scanner — picks up .ini, .yaml, and .yml theme files
// ---------------------------------------------------------------------------

std::vector<ColorScheme> ColorScheme::scanDirectory(const std::string& dir)
{
    std::vector<ColorScheme> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;

    while (dirent* entry = readdir(d)) {
        const std::string name = entry->d_name;
        std::optional<ColorScheme> scheme;

        const auto hasSuffix = [&](std::string_view ext) {
            return name.size() > ext.size() &&
                   name.substr(name.size() - ext.size()) == ext;
        };

        if      (hasSuffix(".ini"))  scheme = loadFromFile(dir + "/" + name);
        else if (hasSuffix(".yaml")) scheme = loadFromYaml(dir + "/" + name);
        else if (hasSuffix(".yml"))  scheme = loadFromYaml(dir + "/" + name);

        if (scheme)
            result.push_back(std::move(*scheme));
    }
    closedir(d);

    std::sort(result.begin(), result.end(), [](const ColorScheme& a, const ColorScheme& b) {
        return a.displayName < b.displayName;
    });
    return result;
}
