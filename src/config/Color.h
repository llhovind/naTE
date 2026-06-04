#pragma once
#include <cstdint>

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb& o) const { return !(*this == o); }
};

// Named color roles for all UI chrome elements (title bars, tab strips, status
// indicators, notification bars, etc.).  Derived at load time from the active
// theme's base16 palette by ColorScheme::deriveUiColors(); default values
// match the Solarized Dark palette so the app is always visually coherent even
// when no theme file is present.
struct UiColors {
    // Frame / grid
    Rgb frameBackground  = {  7,  54,  66 };  // base01 — inter-tile gap + main frame

    // Tile title bars
    Rgb tileActive       = { 38, 139, 210 };  // base0D — focused tile (blue accent)
    Rgb tileInactive     = {101, 123, 131 };  // base03 — unfocused tile
    Rgb tileBroadcast    = {203,  75,  22 };  // base09 — tile in broadcast group

    // Tab strip text and close buttons
    Rgb tabText          = {253, 246, 227 };  // base07 — tab label + "+" text
    Rgb tabCloseActive   = {238, 232, 213 };  // base06 — "×" on active/broadcast tab
    Rgb tabCloseInactive = {101, 123, 131 };  // base03 — "×" on inactive tab

    // Session status badge dots
    Rgb statusUnread        = { 42, 161, 152 };  // base0C — unread output (cyan)
    Rgb statusDisconnected  = {220,  50,  47 };  // base08 — disconnected (red)
    Rgb statusReconnecting  = {181, 137,   0 };  // base0A — reconnecting (yellow)

    // Disconnect notification bar
    Rgb reconnectBarBg   = {203,  75,  22 };  // base09 — warm orange background
    Rgb reconnectBarText = {253, 246, 227 };  // base07 — bar label text

    // Find bar
    Rgb searchBarBg      = {  8,  68,  84 };  // blend(base00×80% + base0D×20%)

    // Indicator control glyphs (WrapControl, AltScrControl, X11Control)
    // "off" glyph color is computed dynamically per tile state in UpdateTitleBarColor().
    Rgb controlActive    = {253, 246, 227 };  // base07 — glyph when feature is on

    // Terminal text selection and search match highlights
    Rgb selectionBg     = { 38, 139, 210 };  // base0D — accent blue (high contrast)
    Rgb selectionFg     = {253, 246, 227 };  // base07 — light text on blue
    Rgb searchMatchBg   = {181, 137,   0 };  // base0A — yellow (warning/match)
    Rgb searchMatchFg   = {  0,  43,  54 };  // base00 — dark text on yellow
    Rgb searchCurrentBg = {203,  75,  22 };  // base09 — orange (current/active match)
};
