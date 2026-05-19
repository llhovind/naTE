#pragma once
#include "config/Color.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

// A named colour theme loaded from a .ini file.
//
// Two formats are supported and may coexist in the same file:
//
//   [Meta]
//   Name=My Theme
//
//   [Colors]                     # naTE two-colour format (fg/bg only)
//   Foreground=131,148,150
//   Background=0,43,54
//
//   [Palette]                    # base16 format — 16 hex values base00-base0F
//   base00=002b36
//   base01=073642
//   ...
//   base0F=d33682
//
// When [Palette] is present, foreground and background are derived from
// base05 and base00 respectively, and the full 16-color ANSI palette is
// computed from the fixed base16-to-ANSI mapping.  A [Colors] section in
// the same file overrides the derived fg/bg values (useful for tweaks).
//
// The stem (filename without .ini) is the identifier used in config.ini.
// The display name comes from [Meta] Name=; if absent the stem is used.
struct ColorScheme {
    std::string stem;         // e.g. "solarized-dark"
    std::string displayName;  // e.g. "Solarized Dark"
    Rgb         foreground = { 131, 148, 150 };  // Solarized Dark base05 fallback
    Rgb         background = {   0,  43,  54 };  // Solarized Dark base00 fallback

    // base16 raw values: palette[0]=base00 … palette[15]=base0F.
    // All zeros when no [Palette] section was found.
    std::array<Rgb, 16> palette   = {};

    // ANSI terminal palette (indices 0–15) derived from palette[] using the
    // canonical base16-to-ANSI mapping.  Falls back to VGA defaults when
    // palette is absent (all zeros).
    std::array<Rgb, 16> ansiColors = {};

    bool hasPalette = false;  // true iff a [Palette] section was parsed

    // Compute ansiColors[] from palette[] using the fixed base16 mapping.
    // Call after loading palette[] if you want ANSI colours.
    void computeAnsiColors();

    // Load a single theme from a .ini file path.
    // The stem is derived from the filename. Returns nullopt on parse failure.
    static std::optional<ColorScheme> loadFromFile(const std::string& path);

    // Write this theme to a .ini file.
    void saveToFile(const std::string& path) const;

    // Scan a directory for *.ini files and return all successfully loaded themes,
    // sorted alphabetically by display name.
    static std::vector<ColorScheme> scanDirectory(const std::string& dir);
};
