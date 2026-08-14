#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config/Config.h"
#include "config/ColorScheme.h"

namespace {

struct TempIni {
    std::filesystem::path path;

    explicit TempIni(std::string_view content) {
        path = std::filesystem::temp_directory_path() / "nate_test_config.ini";
        std::ofstream{path} << content;
    }
    ~TempIni() { std::filesystem::remove(path); }

    std::string stdPath() const { return path.string(); }
};

// A temporary directory that holds a small set of theme files for tests that
// exercise theme resolution.
struct TempThemeDir {
    std::filesystem::path dir;

    TempThemeDir() {
        dir = std::filesystem::temp_directory_path() / "nate_test_themes";
        std::filesystem::create_directories(dir);

        std::ofstream(dir / "solarized-dark.ini")
            << "[Meta]\nName=Solarized Dark\n\n"
            << "[Colors]\nForeground=147,161,161\nBackground=0,43,54\n\n"
            << "[Palette]\n"
            << "base00=002b36\nbase01=073642\nbase02=586e75\nbase03=657b83\n"
            << "base04=839496\nbase05=93a1a1\nbase06=eee8d5\nbase07=fdf6e3\n"
            << "base08=dc322f\nbase09=cb4b16\nbase0A=b58900\nbase0B=859900\n"
            << "base0C=2aa198\nbase0D=268bd2\nbase0E=6c71c4\nbase0F=d33682\n";

        std::ofstream(dir / "solarized-light.ini")
            << "[Meta]\nName=Solarized Light\n\n"
            << "[Colors]\nForeground=88,110,117\nBackground=253,246,227\n\n"
            << "[Palette]\n"
            << "base00=fdf6e3\nbase01=eee8d5\nbase02=93a1a1\nbase03=839496\n"
            << "base04=657b83\nbase05=586e75\nbase06=073642\nbase07=002b36\n"
            << "base08=dc322f\nbase09=cb4b16\nbase0A=b58900\nbase0B=859900\n"
            << "base0C=2aa198\nbase0D=268bd2\nbase0E=6c71c4\nbase0F=d33682\n";
    }
    ~TempThemeDir() { std::filesystem::remove_all(dir); }

    std::string stdPath() const { return dir.string(); }
};

const AppConfig kDefaults;

// Solarized colour constants for test assertions.
constexpr Rgb kSolDarkFg  { 147, 161, 161 };  // base05 of Solarized Dark
constexpr Rgb kSolDarkBg  {   0,  43,  54 };  // base00
constexpr Rgb kSolLightFg {  88, 110, 117 };  // base05 of Solarized Light
constexpr Rgb kSolLightBg { 253, 246, 227 };  // base00

} // namespace

// ---------------------------------------------------------------------------
// Basic load / defaults
// ---------------------------------------------------------------------------

TEST_CASE("given missing file when AppConfig loaded then returns defaults") {
    const auto cfg = AppConfig::load("/nonexistent/path/config.ini");

    REQUIRE(cfg.columns   == kDefaults.columns);
    REQUIRE(cfg.rows      == kDefaults.rows);
    REQUIRE(cfg.fontSize  == kDefaults.fontSize);
    REQUIRE(cfg.textColour == kDefaults.textColour);
    REQUIRE(cfg.bgColour   == kDefaults.bgColour);
}

TEST_CASE("given valid ini when AppConfig loaded then panel values are read") {
    const TempIni ini{
        "[Panel]\n"
        "Columns=100\nRows=40\nFontSize=14\n"
    };
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.columns  == 100);
    REQUIRE(cfg.rows     == 40);
    REQUIRE(cfg.fontSize == 14);
}

TEST_CASE("given partial ini when AppConfig loaded then unspecified values remain defaults") {
    const TempIni ini{"[Panel]\nColumns=132\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.columns  == 132);
    REQUIRE(cfg.rows     == kDefaults.rows);
    REQUIRE(cfg.fontSize == kDefaults.fontSize);
}

TEST_CASE("given empty ini when AppConfig loaded then returns defaults") {
    const TempIni ini{""};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.columns   == kDefaults.columns);
    REQUIRE(cfg.rows      == kDefaults.rows);
    REQUIRE(cfg.fontSize  == kDefaults.fontSize);
    REQUIRE(cfg.textColour == kDefaults.textColour);
    REQUIRE(cfg.bgColour   == kDefaults.bgColour);
}

// ---------------------------------------------------------------------------
// [Session] section
// ---------------------------------------------------------------------------

TEST_CASE("given ini with [Session] DefaultWorkingDir when loaded then field is set")
{
    const TempIni ini{"[Session]\nDefaultWorkingDir=~/work\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultWorkingDir == "~/work");
}

TEST_CASE("given ini with [Session] DefaultLoginShell=true when loaded then flag is true")
{
    const TempIni ini{"[Session]\nDefaultLoginShell=true\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultLoginShell == true);
}

TEST_CASE("given ini with [Session] DefaultLoginShell=false when loaded then flag is false")
{
    const TempIni ini{"[Session]\nDefaultLoginShell=false\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultLoginShell == false);
}

TEST_CASE("given ini with [Session] DefaultEnvFile when loaded then field is set")
{
    const TempIni ini{"[Session]\nDefaultEnvFile=~/.env.work\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultEnvFilePath == "~/.env.work");
}

TEST_CASE("given ini with [Session] one indexed env var pair when loaded then one EnvVar in list")
{
    const TempIni ini{"[Session]\nEnvVar0Key=FOO\nEnvVar0Value=bar\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultEnvVars.size() == 1);
    CHECK(cfg.defaultEnvVars[0].key   == "FOO");
    CHECK(cfg.defaultEnvVars[0].value == "bar");
}

TEST_CASE("given ini with [Session] two indexed env var pairs when loaded then two EnvVars in list")
{
    const TempIni ini{
        "[Session]\n"
        "EnvVar0Key=A\nEnvVar0Value=1\n"
        "EnvVar1Key=B\nEnvVar1Value=2\n"
    };
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultEnvVars.size() == 2);
    CHECK(cfg.defaultEnvVars[0].key   == "A");
    CHECK(cfg.defaultEnvVars[0].value == "1");
    CHECK(cfg.defaultEnvVars[1].key   == "B");
    CHECK(cfg.defaultEnvVars[1].value == "2");
}

TEST_CASE("given ini with [Session] missing EnvVar0 when loaded then defaultEnvVars is empty")
{
    const TempIni ini{"[Session]\nDefaultWorkingDir=~/work\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultEnvVars.empty());
}

TEST_CASE("given ini without [Session] section when loaded then session defaults are default-constructed")
{
    const TempIni ini{"[Panel]\nColumns=80\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.defaultWorkingDir.empty());
    REQUIRE(cfg.defaultEnvFilePath.empty());
    REQUIRE(cfg.defaultLoginShell == false);
    REQUIRE(cfg.defaultEnvVars.empty());
}

// ---------------------------------------------------------------------------
// [Appearance] — theme resolution
// ---------------------------------------------------------------------------

TEST_CASE("given default AppConfig then themeName is solarized-dark and colors are Solarized Dark fallback")
{
    const AppConfig cfg;
    REQUIRE(cfg.themeName  == "solarized-dark");
    REQUIRE(cfg.textColour == kSolDarkFg);
    REQUIRE(cfg.bgColour   == kSolDarkBg);
}

TEST_CASE("given ini with ColorTheme=solarized-dark and themes dir when loaded then colors are Solarized Dark")
{
    const TempIni      ini{"[Appearance]\nColorTheme=solarized-dark\n"};
    const TempThemeDir themes;
    const auto cfg = AppConfig::load(ini.stdPath(), themes.stdPath());
    REQUIRE(cfg.themeName  == "solarized-dark");
    REQUIRE(cfg.textColour == kSolDarkFg);
    REQUIRE(cfg.bgColour   == kSolDarkBg);
}

TEST_CASE("given ini with ColorTheme=solarized-light and themes dir when loaded then colors are Solarized Light")
{
    const TempIni      ini{"[Appearance]\nColorTheme=solarized-light\n"};
    const TempThemeDir themes;
    const auto cfg = AppConfig::load(ini.stdPath(), themes.stdPath());
    REQUIRE(cfg.themeName  == "solarized-light");
    REQUIRE(cfg.textColour == kSolLightFg);
    REQUIRE(cfg.bgColour   == kSolLightBg);
}

TEST_CASE("given ini with legacy ColorTheme=SolarizedDark when loaded then stem is normalised")
{
    const TempIni      ini{"[Appearance]\nColorTheme=SolarizedDark\n"};
    const TempThemeDir themes;
    const auto cfg = AppConfig::load(ini.stdPath(), themes.stdPath());
    REQUIRE(cfg.themeName  == "solarized-dark");
    REQUIRE(cfg.textColour == kSolDarkFg);
}

TEST_CASE("given ini with legacy ColorTheme=SolarizedLight when loaded then stem is normalised")
{
    const TempIni      ini{"[Appearance]\nColorTheme=SolarizedLight\n"};
    const TempThemeDir themes;
    const auto cfg = AppConfig::load(ini.stdPath(), themes.stdPath());
    REQUIRE(cfg.themeName  == "solarized-light");
    REQUIRE(cfg.textColour == kSolLightFg);
}

TEST_CASE("given ini with unknown theme name when loaded then fallback colors are used")
{
    const TempIni      ini{"[Appearance]\nColorTheme=nonexistent-theme\n"};
    const TempThemeDir themes;
    const auto cfg = AppConfig::load(ini.stdPath(), themes.stdPath());
    REQUIRE(cfg.themeName  == "nonexistent-theme");
    REQUIRE(cfg.textColour == kDefaults.textColour);
    REQUIRE(cfg.bgColour   == kDefaults.bgColour);
}

TEST_CASE("given ini with [Appearance] FontFamily and Padding when loaded then values are set")
{
    const TempIni ini{"[Appearance]\nFontFamily=Fira Code\nPadding=8\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.fontFamily == "Fira Code");
    REQUIRE(cfg.padding    == 8);
}

TEST_CASE("given ini with [Behavior] Encoding when loaded then encoding is set")
{
    const TempIni ini{"[Behavior]\nEncoding=UTF-8\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.encoding == "UTF-8");
}

TEST_CASE("given ini with CopyOnSelect=true when loaded then copyOnSelect is true")
{
    const TempIni ini{"[Behavior]\nCopyOnSelect=true\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.copyOnSelect == true);
}

TEST_CASE("given ini with CopyOnSelect=false when loaded then copyOnSelect is false")
{
    const TempIni ini{"[Behavior]\nCopyOnSelect=false\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.copyOnSelect == false);
}

TEST_CASE("given AppConfig with copyOnSelect=true when saved and reloaded then field round-trips")
{
    AppConfig original;
    original.copyOnSelect = true;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_copyonselect.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.copyOnSelect == true);
}

TEST_CASE("given ini with ExternalEditorCommand when loaded then externalEditorCommand is set")
{
    const TempIni ini{"[Behavior]\nExternalEditorCommand=code --wait\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.externalEditorCommand == "code --wait");
}

TEST_CASE("given ini with legacy RemoteEditorCommand when loaded then externalEditorCommand is set")
{
    const TempIni ini{"[Behavior]\nRemoteEditorCommand=vim\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.externalEditorCommand == "vim");
}

TEST_CASE("given AppConfig with externalEditorCommand when saved and reloaded then field round-trips")
{
    AppConfig original;
    original.externalEditorCommand = "nano";

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_externaleditor.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.externalEditorCommand == "nano");
}

TEST_CASE("given ini with ConfirmCloseWindow=false when loaded then confirmCloseWindow is false")
{
    const TempIni ini{"[Behavior]\nConfirmCloseWindow=false\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.confirmCloseWindow == false);
}

TEST_CASE("given no ConfirmCloseWindow key when loaded then confirmCloseWindow defaults to true")
{
    const TempIni ini{"[Behavior]\nCopyOnSelect=true\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.confirmCloseWindow == true);
}

TEST_CASE("given AppConfig with confirmCloseWindow=false when saved and reloaded then field round-trips")
{
    AppConfig original;
    original.confirmCloseWindow = false;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_confirmclose.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.confirmCloseWindow == false);
}

TEST_CASE("given ini with TileLayout=ColumnFirst when loaded then tileLayout is ColumnFirst")
{
    const TempIni ini{"[Appearance]\nTileLayout=ColumnFirst\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.tileLayout == TileLayout::ColumnFirst);
}

TEST_CASE("given ini with TileLayout=RowFirst when loaded then tileLayout is RowFirst")
{
    const TempIni ini{"[Appearance]\nTileLayout=RowFirst\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.tileLayout == TileLayout::RowFirst);
}

TEST_CASE("given ini with unknown TileLayout value when loaded then tileLayout defaults to RowFirst")
{
    const TempIni ini{"[Appearance]\nTileLayout=Mosaic\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.tileLayout == TileLayout::RowFirst);
}

TEST_CASE("given AppConfig with tileLayout=ColumnFirst when saved and reloaded then round-trips")
{
    AppConfig original;
    original.tileLayout = TileLayout::ColumnFirst;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_tilelayout.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.tileLayout == TileLayout::ColumnFirst);
}

TEST_CASE("given AppConfig when saved and reloaded with themes dir then all fields round-trip")
{
    const TempThemeDir themes;

    AppConfig original;
    original.themeName  = "solarized-light";
    original.fontFamily = "Inconsolata";
    original.padding    = 6;
    original.encoding   = "UTF-8";
    original.fontSize   = 14;
    original.columns    = 100;
    original.rows       = 40;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_roundtrip.ini").string();
    original.save(savePath);

    const auto loaded = AppConfig::load(savePath, themes.stdPath());
    std::filesystem::remove(savePath);

    REQUIRE(loaded.themeName  == "solarized-light");
    REQUIRE(loaded.textColour == kSolLightFg);
    REQUIRE(loaded.bgColour   == kSolLightBg);
    REQUIRE(loaded.fontFamily == "Inconsolata");
    REQUIRE(loaded.padding    == 6);
    REQUIRE(loaded.encoding   == "UTF-8");
    REQUIRE(loaded.fontSize   == 14);
    REQUIRE(loaded.columns    == 100);
    REQUIRE(loaded.rows       == 40);
}

TEST_CASE("given ini with solarized-dark theme when loaded then ansiColors are set from palette")
{
    const TempIni      ini{"[Appearance]\nColorTheme=solarized-dark\n"};
    const TempThemeDir themes;
    const auto cfg = AppConfig::load(ini.stdPath(), themes.stdPath());

    // ANSI 0 = base00 = #002b36
    CHECK(cfg.ansiColors[0] == (Rgb{  0,  43,  54}));
    // ANSI 1 = base08 = #dc322f
    CHECK(cfg.ansiColors[1] == (Rgb{220,  50,  47}));
    // ANSI 7 = base05 = #93a1a1
    CHECK(cfg.ansiColors[7] == (Rgb{147, 161, 161}));
    // ANSI 15 = base07 = #fdf6e3
    CHECK(cfg.ansiColors[15] == (Rgb{253, 246, 227}));
}

// ---------------------------------------------------------------------------
// ColorScheme — load / save / scan
// ---------------------------------------------------------------------------

TEST_CASE("given valid theme file when loaded then fields are parsed correctly")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_test_theme.ini").string();
    std::ofstream{path}
        << "[Meta]\nName=My Theme\n\n"
        << "[Colors]\nForeground=10,20,30\nBackground=40,50,60\n";

    const auto scheme = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->stem        == "nate_test_theme");
    CHECK(scheme->displayName == "My Theme");
    CHECK(scheme->foreground  == (Rgb{10, 20, 30}));
    CHECK(scheme->background  == (Rgb{40, 50, 60}));
}

TEST_CASE("given theme file without [Meta] Name when loaded then stem is used as display name")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_unnamed.ini").string();
    std::ofstream{path} << "[Colors]\nForeground=1,2,3\nBackground=4,5,6\n";

    const auto scheme = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->displayName == "nate_unnamed");
}

TEST_CASE("given missing theme file when loaded then nullopt is returned")
{
    const auto scheme = ColorScheme::loadFromFile("/nonexistent/theme.ini");
    REQUIRE_FALSE(scheme.has_value());
}

TEST_CASE("given theme file with no color entries when loaded then nullopt is returned")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_empty_theme.ini").string();
    std::ofstream{path} << "[Meta]\nName=Empty\n";

    const auto scheme = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE_FALSE(scheme.has_value());
}

TEST_CASE("given ColorScheme when saved then file round-trips correctly")
{
    ColorScheme original;
    original.stem        = "my-theme";
    original.displayName = "My Theme";
    original.foreground  = {10, 20, 30};
    original.background  = {40, 50, 60};

    const auto path = (std::filesystem::temp_directory_path()
                       / "my-theme.ini").string();
    original.saveToFile(path);
    const auto loaded = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(loaded.has_value());
    CHECK(loaded->displayName == "My Theme");
    CHECK(loaded->foreground  == (Rgb{10, 20, 30}));
    CHECK(loaded->background  == (Rgb{40, 50, 60}));
}

TEST_CASE("given themes directory when scanned then all valid themes are returned sorted")
{
    const TempThemeDir themes;
    const auto result = ColorScheme::scanDirectory(themes.stdPath());

    REQUIRE(result.size() == 2);
    // scanDirectory sorts by displayName; "Solarized Dark" < "Solarized Light"
    CHECK(result[0].stem        == "solarized-dark");
    CHECK(result[0].displayName == "Solarized Dark");
    CHECK(result[1].stem        == "solarized-light");
    CHECK(result[1].displayName == "Solarized Light");
}

TEST_CASE("given empty directory when scanned then empty vector is returned")
{
    const auto dir = std::filesystem::temp_directory_path() / "nate_empty_themes";
    std::filesystem::create_directories(dir);
    const auto result = ColorScheme::scanDirectory(dir.string());
    std::filesystem::remove_all(dir);
    REQUIRE(result.empty());
}

// ---------------------------------------------------------------------------
// ColorScheme — base16 [Palette] support
// ---------------------------------------------------------------------------

TEST_CASE("given theme file with [Palette] section when loaded then palette is parsed")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_test_palette.ini").string();
    std::ofstream{path}
        << "[Meta]\nName=Palette Theme\n\n"
        << "[Palette]\n"
        << "base00=002b36\nbase01=073642\nbase02=586e75\nbase03=657b83\n"
        << "base04=839496\nbase05=93a1a1\nbase06=eee8d5\nbase07=fdf6e3\n"
        << "base08=dc322f\nbase09=cb4b16\nbase0A=b58900\nbase0B=859900\n"
        << "base0C=2aa198\nbase0D=268bd2\nbase0E=6c71c4\nbase0F=d33682\n";

    const auto scheme = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->hasPalette == true);
    CHECK(scheme->palette[0]  == (Rgb{  0,  43,  54}));  // base00
    CHECK(scheme->palette[5]  == (Rgb{147, 161, 161}));  // base05
    CHECK(scheme->palette[8]  == (Rgb{220,  50,  47}));  // base08
    CHECK(scheme->palette[15] == (Rgb{211,  54, 130}));  // base0F
}

TEST_CASE("given theme file with [Palette] when loaded then fg/bg derived from base05/base00")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_test_palette_fgbg.ini").string();
    std::ofstream{path}
        << "[Palette]\n"
        << "base00=002b36\nbase01=073642\nbase02=586e75\nbase03=657b83\n"
        << "base04=839496\nbase05=93a1a1\nbase06=eee8d5\nbase07=fdf6e3\n"
        << "base08=dc322f\nbase09=cb4b16\nbase0A=b58900\nbase0B=859900\n"
        << "base0C=2aa198\nbase0D=268bd2\nbase0E=6c71c4\nbase0F=d33682\n";

    const auto scheme = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->background == (Rgb{  0,  43,  54}));  // base00
    CHECK(scheme->foreground == (Rgb{147, 161, 161}));  // base05
}

TEST_CASE("given theme file with [Palette] and [Colors] when loaded then Colors overrides derived fg/bg")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_test_palette_override.ini").string();
    std::ofstream{path}
        << "[Colors]\nForeground=10,20,30\nBackground=40,50,60\n\n"
        << "[Palette]\n"
        << "base00=002b36\nbase01=073642\nbase02=586e75\nbase03=657b83\n"
        << "base04=839496\nbase05=93a1a1\nbase06=eee8d5\nbase07=fdf6e3\n"
        << "base08=dc322f\nbase09=cb4b16\nbase0A=b58900\nbase0B=859900\n"
        << "base0C=2aa198\nbase0D=268bd2\nbase0E=6c71c4\nbase0F=d33682\n";

    const auto scheme = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->foreground == (Rgb{10, 20, 30}));
    CHECK(scheme->background == (Rgb{40, 50, 60}));
}

TEST_CASE("given ColorScheme with palette when saved then palette round-trips correctly")
{
    ColorScheme original;
    original.stem        = "pal-theme";
    original.displayName = "Pal Theme";
    original.foreground  = {147, 161, 161};
    original.background  = {  0,  43,  54};
    original.hasPalette  = true;
    original.palette[0]  = {  0,  43,  54};
    original.palette[5]  = {147, 161, 161};
    original.palette[8]  = {220,  50,  47};
    original.palette[15] = {253, 246, 227};

    const auto path = (std::filesystem::temp_directory_path()
                       / "pal-theme.ini").string();
    original.saveToFile(path);
    const auto loaded = ColorScheme::loadFromFile(path);
    std::filesystem::remove(path);

    REQUIRE(loaded.has_value());
    CHECK(loaded->hasPalette  == true);
    CHECK(loaded->palette[0]  == (Rgb{  0,  43,  54}));
    CHECK(loaded->palette[5]  == (Rgb{147, 161, 161}));
    CHECK(loaded->palette[8]  == (Rgb{220,  50,  47}));
    CHECK(loaded->palette[15] == (Rgb{253, 246, 227}));
}

// ---------------------------------------------------------------------------
// ColorScheme — YAML loader
// ---------------------------------------------------------------------------

// Flat v0.x format: scheme: / baseXX: at top level, quoted hex values.
TEST_CASE("given base16 yaml v0 file when loaded then palette and name are parsed")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_test_flat.yaml").string();
    std::ofstream{path}
        << "scheme: \"Solarized Dark\"\n"
        << "author: \"Ethan Schoonover\"\n"
        << "base00: \"002b36\"\n"
        << "base01: \"073642\"\n"
        << "base02: \"586e75\"\n"
        << "base03: \"657b83\"\n"
        << "base04: \"839496\"\n"
        << "base05: \"93a1a1\"\n"
        << "base06: \"eee8d5\"\n"
        << "base07: \"fdf6e3\"\n"
        << "base08: \"dc322f\"\n"
        << "base09: \"cb4b16\"\n"
        << "base0A: \"b58900\"\n"
        << "base0B: \"859900\"\n"
        << "base0C: \"2aa198\"\n"
        << "base0D: \"268bd2\"\n"
        << "base0E: \"6c71c4\"\n"
        << "base0F: \"d33682\"\n";

    const auto scheme = ColorScheme::loadFromYaml(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->displayName    == "Solarized Dark");
    CHECK(scheme->hasPalette     == true);
    CHECK(scheme->hasPaletteSection == true);
    CHECK(scheme->palette[0]  == (Rgb{  0,  43,  54}));  // base00
    CHECK(scheme->palette[5]  == (Rgb{147, 161, 161}));  // base05
    CHECK(scheme->palette[13] == (Rgb{ 38, 139, 210}));  // base0D
    // fg/bg derived from palette
    CHECK(scheme->background  == scheme->palette[0]);
    CHECK(scheme->foreground  == scheme->palette[5]);
}

// Tinted-theming v2 format: name: / palette: block with indented keys.
TEST_CASE("given base16 yaml v2 file with palette block when loaded then palette and name are parsed")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_test_v2.yaml").string();
    std::ofstream{path}
        << "system: \"base16\"\n"
        << "name: \"Gruvbox Dark\"\n"
        << "author: \"Dawid Kurek\"\n"
        << "variant: \"dark\"\n"
        << "palette:\n"
        << "  base00: \"282828\"\n"
        << "  base01: \"3c3836\"\n"
        << "  base02: \"504945\"\n"
        << "  base03: \"665c54\"\n"
        << "  base04: \"bdae93\"\n"
        << "  base05: \"d5c4a1\"\n"
        << "  base06: \"ebdbb2\"\n"
        << "  base07: \"fbf1c7\"\n"
        << "  base08: \"fb4934\"\n"
        << "  base09: \"fe8019\"\n"
        << "  base0A: \"fabd2f\"\n"
        << "  base0B: \"b8bb26\"\n"
        << "  base0C: \"8ec07c\"\n"
        << "  base0D: \"83a598\"\n"
        << "  base0E: \"d3869b\"\n"
        << "  base0F: \"d65d0e\"\n";

    const auto scheme = ColorScheme::loadFromYaml(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->displayName == "Gruvbox Dark");
    CHECK(scheme->palette[0]  == (Rgb{ 40,  40,  40}));  // base00 282828
    CHECK(scheme->palette[8]  == (Rgb{251,  73,  52}));  // base08 fb4934
    CHECK(scheme->background  == scheme->palette[0]);
    CHECK(scheme->foreground  == scheme->palette[5]);
}

TEST_CASE("given yaml file with no palette entries when loaded then nullopt is returned")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_empty.yaml").string();
    std::ofstream{path} << "scheme: \"Empty\"\nauthor: \"Nobody\"\n";

    const auto scheme = ColorScheme::loadFromYaml(path);
    std::filesystem::remove(path);

    REQUIRE_FALSE(scheme.has_value());
}

TEST_CASE("given yaml file with hash-prefixed hex values when loaded then values are parsed")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_hash.yaml").string();
    std::ofstream{path}
        << "scheme: \"Hash Theme\"\n"
        << "base00: \"#1a1a2e\"\n"
        << "base05: \"#e0e0e0\"\n";

    const auto scheme = ColorScheme::loadFromYaml(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->palette[0] == (Rgb{26, 26, 46}));
    CHECK(scheme->palette[5] == (Rgb{224, 224, 224}));
}

TEST_CASE("given yaml file with inline comments when loaded then comments are stripped and palette is parsed")
{
    const auto path = (std::filesystem::temp_directory_path()
                       / "nate_inline_comments.yaml").string();
    std::ofstream{path}
        << "system: \"base16\"\n"
        << "name: \"Humanoid light\"\n"
        << "author: \"Thomas Friese\" # author comment\n"
        << "variant: \"light\"\n"
        << "palette:\n"
        << "  base00: \"f8f8f2\" # #f8f8f2 ----\n"
        << "  base05: \"232629\" # #232629 ++\n"
        << "  base08: \"b0151a\" # #b0151a red\n"
        << "  base01: \"efefe9\"\n"
        << "  base02: \"deded8\"\n"
        << "  base03: \"c0c0bd\"\n"
        << "  base04: \"60615d\"\n"
        << "  base06: \"2f3337\"\n"
        << "  base07: \"070708\"\n"
        << "  base09: \"ff3d00\"\n"
        << "  base0A: \"ffb627\"\n"
        << "  base0B: \"388e3c\"\n"
        << "  base0C: \"008e8e\"\n"
        << "  base0D: \"0082c9\"\n"
        << "  base0E: \"700f98\"\n"
        << "  base0F: \"b27701\"\n";

    const auto scheme = ColorScheme::loadFromYaml(path);
    std::filesystem::remove(path);

    REQUIRE(scheme.has_value());
    CHECK(scheme->displayName == "Humanoid light");
    CHECK(scheme->palette[0] == (Rgb{0xf8, 0xf8, 0xf2}));
    CHECK(scheme->palette[5] == (Rgb{0x23, 0x26, 0x29}));
    CHECK(scheme->palette[8] == (Rgb{0xb0, 0x15, 0x1a}));
}

TEST_CASE("given missing yaml file when loadFromYaml called then nullopt is returned")
{
    const auto scheme = ColorScheme::loadFromYaml("/nonexistent/theme.yaml");
    REQUIRE_FALSE(scheme.has_value());
}

TEST_CASE("given directory with yaml themes when scanned then yaml themes are included")
{
    const auto dir = std::filesystem::temp_directory_path() / "nate_yaml_themes";
    std::filesystem::create_directories(dir);

    std::ofstream(dir / "gruvbox.yaml")
        << "scheme: \"Gruvbox\"\n"
        << "base00: \"282828\"\nbase01: \"3c3836\"\nbase02: \"504945\"\nbase03: \"665c54\"\n"
        << "base04: \"bdae93\"\nbase05: \"d5c4a1\"\nbase06: \"ebdbb2\"\nbase07: \"fbf1c7\"\n"
        << "base08: \"fb4934\"\nbase09: \"fe8019\"\nbase0A: \"fabd2f\"\nbase0B: \"b8bb26\"\n"
        << "base0C: \"8ec07c\"\nbase0D: \"83a598\"\nbase0E: \"d3869b\"\nbase0F: \"d65d0e\"\n";

    std::ofstream(dir / "minimal.ini")
        << "[Meta]\nName=Minimal\n\n[Colors]\nForeground=200,200,200\nBackground=10,10,10\n";

    const auto result = ColorScheme::scanDirectory(dir.string());
    std::filesystem::remove_all(dir);

    REQUIRE(result.size() == 2);
    // sorted: "Gruvbox" < "Minimal"
    CHECK(result[0].stem        == "gruvbox");
    CHECK(result[0].displayName == "Gruvbox");
    CHECK(result[1].displayName == "Minimal");
}

// ---------------------------------------------------------------------------
// cursorBlink
// ---------------------------------------------------------------------------

TEST_CASE("given default AppConfig then cursorBlink is true")
{
    const AppConfig cfg;
    REQUIRE(cfg.cursorBlink == true);
}

TEST_CASE("given ini with CursorBlink=false when loaded then cursorBlink is false")
{
    const TempIni ini{"[Appearance]\nCursorBlink=false\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.cursorBlink == false);
}

TEST_CASE("given ini with CursorBlink=true when loaded then cursorBlink is true")
{
    const TempIni ini{"[Appearance]\nCursorBlink=true\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.cursorBlink == true);
}

TEST_CASE("given AppConfig with cursorBlink=false when saved and reloaded then field round-trips")
{
    AppConfig original;
    original.cursorBlink = false;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_cursorblink.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.cursorBlink == false);
}

// ---------------------------------------------------------------------------
// bellMode
// ---------------------------------------------------------------------------

TEST_CASE("given default AppConfig then bellMode is Visual")
{
    const AppConfig cfg;
    REQUIRE(cfg.bellMode == BellMode::Visual);
}

TEST_CASE("given ini with BellMode=None when loaded then bellMode is None")
{
    const TempIni ini{"[Behavior]\nBellMode=None\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.bellMode == BellMode::None);
}

TEST_CASE("given ini with BellMode=Audible when loaded then bellMode is Audible")
{
    const TempIni ini{"[Behavior]\nBellMode=Audible\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.bellMode == BellMode::Audible);
}

TEST_CASE("given ini with BellMode=Visual when loaded then bellMode is Visual")
{
    const TempIni ini{"[Behavior]\nBellMode=Visual\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.bellMode == BellMode::Visual);
}

TEST_CASE("given ini with unknown BellMode when loaded then bellMode defaults to Visual")
{
    const TempIni ini{"[Behavior]\nBellMode=Unrecognised\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.bellMode == BellMode::Visual);
}

TEST_CASE("given AppConfig with bellMode=Audible when saved and reloaded then field round-trips")
{
    AppConfig original;
    original.bellMode = BellMode::Audible;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_bellmode.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.bellMode == BellMode::Audible);
}

TEST_CASE("given AppConfig with bellMode=None when saved and reloaded then field round-trips")
{
    AppConfig original;
    original.bellMode = BellMode::None;

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_bellmode_none.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.bellMode == BellMode::None);
}

// ---------------------------------------------------------------------------
// [Terminal] GeometryPresets
// ---------------------------------------------------------------------------

TEST_CASE("given default AppConfig then geometryPresets are 80x24 and 132x24")
{
    const AppConfig cfg;
    REQUIRE(cfg.geometryPresets.size() == 2);
    CHECK(cfg.geometryPresets[0].cols == 80);
    CHECK(cfg.geometryPresets[0].rows == 24);
    CHECK(cfg.geometryPresets[1].cols == 132);
    CHECK(cfg.geometryPresets[1].rows == 24);
}

TEST_CASE("given GeometryPresets CSV when parsed then yields ordered presets")
{
    const TempIni ini{"[Terminal]\nGeometryPresets=80x24,110x24,120x40,132x24\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.geometryPresets.size() == 4);
    CHECK(cfg.geometryPresets[0].cols == 80);
    CHECK(cfg.geometryPresets[0].rows == 24);
    CHECK(cfg.geometryPresets[1].cols == 110);
    CHECK(cfg.geometryPresets[2].cols == 120);
    CHECK(cfg.geometryPresets[2].rows == 40);
    CHECK(cfg.geometryPresets[3].cols == 132);
}

TEST_CASE("given empty GeometryPresets value when parsed then falls back to defaults")
{
    const TempIni ini{"[Terminal]\nGeometryPresets=\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.geometryPresets.size() == 2);
    CHECK(cfg.geometryPresets[0].cols == 80);
    CHECK(cfg.geometryPresets[1].cols == 132);
}

TEST_CASE("given garbage GeometryPresets value when parsed then falls back to defaults")
{
    const TempIni ini{"[Terminal]\nGeometryPresets=foo,12,xy,0x0\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.geometryPresets.size() == 2);
    CHECK(cfg.geometryPresets[0].cols == 80);
    CHECK(cfg.geometryPresets[1].cols == 132);
}

TEST_CASE("given AppConfig with custom geometryPresets when saved and reloaded then round-trips")
{
    AppConfig original;
    original.geometryPresets = {{80, 24}, {100, 30}, {200, 50}};

    const auto savePath = (std::filesystem::temp_directory_path()
                           / "nate_test_geometrypresets.ini").string();
    original.save(savePath);
    const auto loaded = AppConfig::load(savePath);
    std::filesystem::remove(savePath);

    REQUIRE(loaded.geometryPresets.size() == 3);
    CHECK(loaded.geometryPresets[0].cols == 80);
    CHECK(loaded.geometryPresets[0].rows == 24);
    CHECK(loaded.geometryPresets[1].cols == 100);
    CHECK(loaded.geometryPresets[1].rows == 30);
    CHECK(loaded.geometryPresets[2].cols == 200);
    CHECK(loaded.geometryPresets[2].rows == 50);
}

// ---------------------------------------------------------------------------

TEST_CASE("given ColorScheme with palette when computeAnsiColors called then ANSI mapping is correct")
{
    ColorScheme scheme;
    scheme.hasPalette = true;
    // Set only the slots we care about for the canonical mapping.
    scheme.palette[0]  = {  0,  43,  54};  // base00 → ANSI 0
    scheme.palette[3]  = {101, 123, 131};  // base03 → ANSI 8
    scheme.palette[5]  = {147, 161, 161};  // base05 → ANSI 7
    scheme.palette[7]  = {253, 246, 227};  // base07 → ANSI 15
    scheme.palette[8]  = {220,  50,  47};  // base08 → ANSI 1, 9
    scheme.palette[10] = {181, 137,   0};  // base0A → ANSI 3, 11
    scheme.palette[11] = {133, 153,   0};  // base0B → ANSI 2, 10
    scheme.palette[12] = { 42, 161, 152};  // base0C → ANSI 6, 14
    scheme.palette[13] = { 38, 139, 210};  // base0D → ANSI 4, 12
    scheme.palette[14] = {108, 113, 196};  // base0E → ANSI 5, 13
    scheme.computeAnsiColors();

    CHECK(scheme.ansiColors[0]  == (Rgb{  0,  43,  54}));  // black  = base00
    CHECK(scheme.ansiColors[1]  == (Rgb{220,  50,  47}));  // red    = base08
    CHECK(scheme.ansiColors[7]  == (Rgb{147, 161, 161}));  // white  = base05
    CHECK(scheme.ansiColors[8]  == (Rgb{101, 123, 131}));  // br.blk = base03
    CHECK(scheme.ansiColors[9]  == (Rgb{220,  50,  47}));  // br.red = base08
    CHECK(scheme.ansiColors[15] == (Rgb{253, 246, 227}));  // br.wht = base07
}

// ---------------------------------------------------------------------------
// Symlink policy
// ---------------------------------------------------------------------------

TEST_CASE("given a default config when constructed then links are preserved")
{
    const AppConfig cfg;
    REQUIRE(cfg.symlinkPolicy == term::fs::SymlinkPolicy::Preserve);
}

TEST_CASE("given ini with SymlinkPolicy=skip when loaded then links are skipped")
{
    const TempIni ini{"[Behavior]\nSymlinkPolicy=skip\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.symlinkPolicy == term::fs::SymlinkPolicy::Skip);
}

TEST_CASE("given ini naming a policy nothing implements when loaded then the default stands")
{
    // "follow" is reserved in the enum but unimplemented. Accepting it would
    // mean silently doing something other than what the file asked for.
    const TempIni ini{"[Behavior]\nSymlinkPolicy=follow\n"};
    const auto cfg = AppConfig::load(ini.stdPath());
    REQUIRE(cfg.symlinkPolicy == term::fs::SymlinkPolicy::Preserve);
}

TEST_CASE("given a symlink policy when saved and reloaded then it survives the round trip")
{
    const TempIni ini{""};
    AppConfig cfg;
    cfg.symlinkPolicy = term::fs::SymlinkPolicy::Skip;
    cfg.save(ini.stdPath());

    const auto reloaded = AppConfig::load(ini.stdPath());
    REQUIRE(reloaded.symlinkPolicy == term::fs::SymlinkPolicy::Skip);
}

// ---------------------------------------------------------------------------
// File explorer window position and column widths
// ---------------------------------------------------------------------------

TEST_CASE("given ini with explorer position when loaded then coordinates are read")
{
    const TempIni ini{"[Behavior]\nFileExplorerX=240\nFileExplorerY=130\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.fileExplorerX == 240);
    REQUIRE(cfg.fileExplorerY == 130);
}

TEST_CASE("given ini with no explorer position when loaded then position is unset")
{
    // Not 0,0: an absent position must stay distinguishable from the top-left
    // corner, or every fresh config would pin the window there.
    const TempIni ini{"[Behavior]\nFileExplorerWidth=800\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.fileExplorerX == kUnsetWindowCoord);
    REQUIRE(cfg.fileExplorerY == kUnsetWindowCoord);
}

TEST_CASE("given ini with empty explorer position when loaded then position is unset")
{
    // What save() writes for a window that has never been placed.
    const TempIni ini{"[Behavior]\nFileExplorerX=\nFileExplorerY=\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.fileExplorerX == kUnsetWindowCoord);
    REQUIRE(cfg.fileExplorerY == kUnsetWindowCoord);
}

TEST_CASE("given ini with negative explorer position when loaded then the coordinates survive")
{
    // A display arranged left of or above the primary one has negative
    // coordinates; they must not be mistaken for absent values.
    const TempIni ini{"[Behavior]\nFileExplorerX=-1700\nFileExplorerY=-40\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.fileExplorerX == -1700);
    REQUIRE(cfg.fileExplorerY == -40);
}

TEST_CASE("given ini with explorer column widths when loaded then every width is read")
{
    const TempIni ini{"[Behavior]\nFileExplorerColumnWidths=300,80,140,100,120\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    const std::array<int, kFileExplorerColumnCount> expected{300, 80, 140, 100, 120};
    REQUIRE(cfg.fileExplorerColumnWidths == expected);
}

TEST_CASE("given ini with too few explorer column widths when loaded then the rest keep defaults")
{
    // A hand-edited or truncated line costs the user the columns it omits,
    // not all five.
    const TempIni ini{"[Behavior]\nFileExplorerColumnWidths=300,80\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.fileExplorerColumnWidths[0] == 300);
    REQUIRE(cfg.fileExplorerColumnWidths[1] == 80);
    REQUIRE(cfg.fileExplorerColumnWidths[2] == kDefaultFileExplorerColumnWidths[2]);
    REQUIRE(cfg.fileExplorerColumnWidths[4] == kDefaultFileExplorerColumnWidths[4]);
}

TEST_CASE("given ini with an unusable explorer column width when loaded then that column keeps its default")
{
    // Zero and negative widths are refused: a zero-width column is invisible
    // and cannot be dragged back into view.
    const TempIni ini{"[Behavior]\nFileExplorerColumnWidths=300,0,-5,bad,120\n"};
    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.fileExplorerColumnWidths[0] == 300);
    REQUIRE(cfg.fileExplorerColumnWidths[1] == kDefaultFileExplorerColumnWidths[1]);
    REQUIRE(cfg.fileExplorerColumnWidths[2] == kDefaultFileExplorerColumnWidths[2]);
    REQUIRE(cfg.fileExplorerColumnWidths[3] == kDefaultFileExplorerColumnWidths[3]);
    REQUIRE(cfg.fileExplorerColumnWidths[4] == 120);
}

TEST_CASE("given a placed explorer window when config is saved then position and widths round-trip")
{
    const TempIni ini{""};

    AppConfig cfg;
    cfg.fileExplorerX            = -1700;
    cfg.fileExplorerY            = 130;
    cfg.fileExplorerWidth        = 880;
    cfg.fileExplorerHeight       = 640;
    cfg.fileExplorerColumnWidths = {300, 80, 140, 100, 120};
    cfg.save(ini.stdPath());

    const auto reloaded = AppConfig::load(ini.stdPath());

    REQUIRE(reloaded.fileExplorerX            == cfg.fileExplorerX);
    REQUIRE(reloaded.fileExplorerY            == cfg.fileExplorerY);
    REQUIRE(reloaded.fileExplorerWidth        == cfg.fileExplorerWidth);
    REQUIRE(reloaded.fileExplorerHeight       == cfg.fileExplorerHeight);
    REQUIRE(reloaded.fileExplorerColumnWidths == cfg.fileExplorerColumnWidths);
}

TEST_CASE("given an unplaced explorer window when config is saved then it reloads as unplaced")
{
    // The default config must survive a save/load cycle without acquiring a
    // position it was never given.
    const TempIni ini{""};

    AppConfig cfg;
    cfg.save(ini.stdPath());

    const auto reloaded = AppConfig::load(ini.stdPath());

    REQUIRE(reloaded.fileExplorerX == kUnsetWindowCoord);
    REQUIRE(reloaded.fileExplorerY == kUnsetWindowCoord);
}
