#pragma once
#include "config/Color.h"     // Rgb, UiColors
#include "fs/SymlinkPolicy.h"
#include "transport/EnvVar.h"
#include <array>
#include <limits>
#include <string>
#include <vector>

struct GeometryPreset {
    unsigned short cols = 80;
    unsigned short rows = 24;
};

enum class TileLayout  { RowFirst, ColumnFirst };
enum class CursorStyle { Block, Bar, Underline };
enum class BellMode    { None, Visual, Audible };

// The two shapes the file explorer window takes. Explore gives the whole
// window to one listing for routine browsing; Transfer adds the second pane,
// the copy buttons and the transfer queue. Named rather than a bool because
// "true" would have to be read as two panes *and* buttons *and* queue panel.
enum class FileExplorerMode { Explore, Transfer };

// Columns in a file explorer listing. Mirrors ui::FileColumnCount, which cannot
// be named from here because this layer stays wx-free; a static_assert in
// ui/RemoteFileListCtrl.h fails the build if the two ever disagree.
inline constexpr int kFileExplorerColumnCount = 5;

// Starting width of each column, in pixels, in the order Name, Size, Modified,
// Permissions, Owner. Defined here rather than at the InsertColumn calls so the
// value a user's saved widths fall back to and the value a fresh listing starts
// from are the same number.
inline constexpr std::array<int, kFileExplorerColumnCount>
    kDefaultFileExplorerColumnWidths = {240, 90, 130, 105, 110};

// "This window has never been placed", for a saved window coordinate. A
// sentinel is needed because 0 and negative coordinates are both legal: a
// display arranged left of the primary one has negative x throughout.
inline constexpr int kUnsetWindowCoord = std::numeric_limits<int>::min();

// The parts of a file explorer window's appearance that outlive it closing.
//
// Reported as one struct rather than as loose arguments: the window has four
// numbers and a column array to hand back, and a callback taking nine ints
// invites transpositions the compiler cannot catch.
struct FileExplorerLayout {
    int width  = 0;
    int height = 0;
    // Top-left of the frame, or kUnsetWindowCoord when it should not be
    // recorded — a maximised window's position is not one the user chose.
    int x = kUnsetWindowCoord;
    int y = kUnsetWindowCoord;
    std::array<int, kFileExplorerColumnCount> columnWidths =
        kDefaultFileExplorerColumnWidths;
};

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
    bool        cursorBlink = true;

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

    // UI chrome colors derived from the active theme's base16 palette.
    // Defaults are Solarized Dark so the app is visually coherent even when
    // no theme file is present.
    UiColors uiColors = {};

    // [Appearance]
    std::string fontFamily  = "";              // empty = system monospace
    int         padding     = 4;              // px inset around terminal canvas
    std::string themeName   = "solarized-dark";  // stem of theme file
    TileLayout  tileLayout  = TileLayout::RowFirst;

    // [Behavior]
    std::string encoding         = "UTF-8";
    std::string webSearchUrl     = "https://duckduckgo.com/?q=";
    std::string wordSelectRegex  = "[^\\s]+"; // regex matching a "word" for double-click selection
    BellMode    bellMode           = BellMode::Visual;
    bool        copyOnSelect           = true;   // copy to X11 primary selection on mouseup
    bool        confirmCloseWindow     = true;   // false = suppress all close-confirmation dialogs
    // Starting policy for a file explorer window; each window can change its
    // own without disturbing this.
    term::fs::SymlinkPolicy symlinkPolicy = term::fs::SymlinkPolicy::Preserve;

    // File explorer window geometry, remembered application-wide rather than
    // per session: the window is about a task, and a user who sizes it once
    // wants that size the next time whatever the session happens to be.
    //
    // The mode is deliberately absent. Both menu entries name the mode they
    // open in, so a stored value would have no reader. Neither is the sash:
    // the window is sized in whole panes, and Transfer mode splits its width
    // in two, so there is no independent split to remember.
    //
    // Nor are the sort column, the show-hidden toggle or the name filter. Those
    // are view state a window deliberately starts fresh on — see
    // kDefaultSortKey and kDefaultShowHidden in fs/DirModel.h for why, and for
    // what promoting either to a preference would take.
    int         fileExplorerWidth      = 720;    // one pane; Transfer mode doubles it
    int         fileExplorerHeight     = 700;
    // Where the window last stood. kUnsetWindowCoord until the user has placed
    // one, which leaves the first window wherever the window manager puts it —
    // a better guess than any coordinate this application could invent.
    int         fileExplorerX          = kUnsetWindowCoord;
    int         fileExplorerY          = kUnsetWindowCoord;
    // Listing column widths, shared by every pane in every explorer window: the
    // columns show the same five things everywhere, so a width chosen once is a
    // width chosen for all of them.
    std::array<int, kFileExplorerColumnCount> fileExplorerColumnWidths =
        kDefaultFileExplorerColumnWidths;
    std::string externalEditorCommand  = "";     // empty = $EDITOR; e.g. "code --wait"

    // [Session] defaults — applied to every new session, overridable per profile
    std::string                        defaultShell;        // empty = $SHELL → /bin/sh
    std::string                        defaultWorkingDir;   // empty = inherit launcher cwd
    std::vector<term::transport::EnvVar> defaultEnvVars;
    std::string                        defaultEnvFilePath;
    bool                               defaultLoginShell  = false;
    bool                               defaultWrapMode    = false;

    // [Restore] session-restore behaviour
    bool autoRestoreSession  = false;
    int  sessionSaveInterval = 300;    // seconds; 0 = disabled
    bool saveScrollbackWithWorkspace = false;
    int  scrollbackSaveLines         = 10'000;
    bool scrollbackSaveStyles        = true;

    // themesDir is the user themes directory (~/.nate/themes).
    // When empty or the named theme file is missing, textColour/bgColour
    // keep their hardcoded fallback values.
    static AppConfig load(const std::string& configPath,
                          const std::string& themesDir = "");
    void             save(const std::string& configPath) const;
};
