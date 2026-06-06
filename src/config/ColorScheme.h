#pragma once
#include "config/Color.h"
#include <array>
#include <optional>
#include <string>
#include <vector>

// A named colour theme loaded from a .ini file.
//
// Two palette formats are supported and may coexist in the same file:
//
//   [Meta]
//   Name=My Theme
//
//   [Colors]                     # naTE two-colour format (fg/bg only)
//   Foreground=131,148,150
//   Background=0,43,54
//
//   [Palette]                    # base16 format — 16 hex values base00-base0F
//   base00=002b36                # regular and bright colours share base16 slots
//   ...
//   base0F=d33682
//
//   [ANSI]                       # direct format — all 16 ANSI indices individually
//   0=000000                     # lets regular and bright variants differ freely
//   1=800000
//   ...
//   15=ffffff
//
// [ANSI] takes precedence over [Palette] when both are present.
// When [Palette] is present (and no [ANSI]), foreground and background are derived
// from base05 and base00 respectively.  A [Colors] section overrides fg/bg in
// either case.
//
// The stem (filename without .ini) is the identifier used in config.ini.
// The display name comes from [Meta] Name=; if absent the stem is used.
struct ColorScheme {
    std::string stem;         // e.g. "solarized-dark"
    std::string displayName;  // e.g. "Solarized Dark"
    Rgb         foreground = { 131, 148, 150 };  // Solarized Dark base05 fallback
    Rgb         background = {   0,  43,  54 };  // Solarized Dark base00 fallback
    Rgb         cursor     = { 131, 148, 150 };  // Solarized Dark base05 fallback

    // base16 raw values: palette[0]=base00 … palette[15]=base0F.
    // All zeros when no [Palette] section was found.
    std::array<Rgb, 16> palette   = {};

    // ANSI terminal palette (indices 0–15) derived from palette[] using the
    // canonical base16-to-ANSI mapping.  Falls back to VGA defaults when
    // palette is absent (all zeros).
    std::array<Rgb, 16> ansiColors = {};

    bool hasPalette        = false;  // true iff a [Palette] or [ANSI] section was parsed
    bool hasDirectAnsi     = false;  // true iff an [ANSI] section specified colors directly
    bool hasPaletteSection = false;  // true iff a [Palette] section populated palette[]

    // Compute ansiColors[] from palette[] using the fixed base16 mapping.
    // Call after loading palette[] if you want ANSI colours.
    void computeAnsiColors();

    // Derive UiColors (chrome element color roles) from the active palette.
    // Uses the base16 palette when hasPaletteSection is true, falls back to
    // ANSI semantic slots when hasDirectAnsi is true, and returns Solarized Dark
    // defaults only for themes with no palette at all.
    UiColors deriveUiColors() const;

    // Load a single theme from a .ini file path.
    // The stem is derived from the filename. Returns nullopt on parse failure.
    static std::optional<ColorScheme> loadFromFile(const std::string& path);

    // Load a base16 theme from a YAML file (.yaml / .yml).
    // Supports both the flat v0.x format (scheme: / baseXX: at top level) and the
    // tinted-theming v2 format (name: / palette: block with indented baseXX: keys).
    // Returns nullopt when no palette entries are found.
    static std::optional<ColorScheme> loadFromYaml(const std::string& path);

    // Write this theme to a .ini file.
    void saveToFile(const std::string& path) const;

    // Scan a directory for *.ini, *.yaml, and *.yml theme files and return all
    // successfully loaded themes, sorted alphabetically by display name.
    static std::vector<ColorScheme> scanDirectory(const std::string& dir);
};
