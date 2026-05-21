#pragma once
#include "config/Color.h"
#include "session/EnvVar.h"
#include <array>
#include <string>
#include <vector>

struct GeometryPreset {
    unsigned short cols = 80;
    unsigned short rows = 24;
};

enum class CursorStyle { Block, Bar, Underline };

struct AppConfig {
    int columns         = 80;
    int rows            = 24;
    int fontSize        = 12;
    int scrollbackLines = 100'000;
    int ptyLineWidth    = 1024;

    // Resolved from the active theme file at load time.
    // Fallback values are Solarized Dark so the app always has sensible colours
    // even if the theme file is missing.
    Rgb textColour   = { 147, 161, 161 };  // Solarized Dark base05
    Rgb bgColour     = {   0,  43,  54 };  // Solarized Dark base00
    Rgb cursorColour = { 147, 161, 161 };  // Solarized Dark base05
    CursorStyle cursorStyle = CursorStyle::Block;

    // ANSI 16-color palette (indices 0-15) derived from the loaded theme.
    // Defaults are the Solarized Dark base16 mapping.
    std::array<Rgb, 16> ansiColors = {{
        {  0,  43,  54},  //  0 black         base00
        {220,  50,  47},  //  1 red           base08
        {133, 153,   0},  //  2 green         base0B
        {181, 137,   0},  //  3 yellow        base0A
        { 38, 139, 210},  //  4 blue          base0D
        {108, 113, 196},  //  5 magenta       base0E
        { 42, 161, 152},  //  6 cyan          base0C
        {147, 161, 161},  //  7 white         base05
        {101, 123, 131},  //  8 bright black  base03
        {220,  50,  47},  //  9 bright red    base08
        {133, 153,   0},  // 10 bright green  base0B
        {181, 137,   0},  // 11 bright yellow base0A
        { 38, 139, 210},  // 12 bright blue   base0D
        {108, 113, 196},  // 13 bright magenta base0E
        { 42, 161, 152},  // 14 bright cyan   base0C
        {253, 246, 227},  // 15 bright white  base07
    }};

    std::vector<GeometryPreset> geometryPresets = {{80, 24}, {132, 24}};

    // [Appearance]
    std::string fontFamily = "";       // empty = system monospace
    int         padding    = 4;        // px inset around terminal canvas
    std::string themeName  = "solarized-dark";  // stem of theme file

    // [Behavior]
    std::string encoding      = "UTF-8";
    std::string webSearchUrl  = "https://duckduckgo.com/?q=";
    bool        copyOnSelect  = true;    // copy to X11 primary selection on mouseup

    // [Session] defaults — applied to every new session, overridable per profile
    std::string                        defaultShell;        // empty = $SHELL → /bin/sh
    std::string                        defaultWorkingDir;   // empty = inherit launcher cwd
    std::vector<term::session::EnvVar> defaultEnvVars;
    std::string                        defaultEnvFilePath;
    bool                               defaultLoginShell  = false;
    bool                               defaultWrapMode    = false;

    // [Restore] session-restore behaviour
    bool autoRestoreSession  = false;
    int  sessionSaveInterval = 300;    // seconds; 0 = disabled

    // themesDir is the user themes directory (~/.nate/themes).
    // When empty or the named theme file is missing, textColour/bgColour
    // keep their hardcoded fallback values.
    static AppConfig load(const std::string& configPath,
                          const std::string& themesDir = "");
    void             save(const std::string& configPath) const;
};
