#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "Config.h"

namespace {

struct TempIni {
    std::filesystem::path path;

    explicit TempIni(std::string_view content) {
        path = std::filesystem::temp_directory_path() / "nate_test_config.ini";
        std::ofstream{path} << content;
    }
    ~TempIni() { std::filesystem::remove(path); }

    wxString wxPath() const { return wxString(path.string()); }
};

const AppConfig kDefaults;

} // namespace

TEST_CASE("given missing file when AppConfig loaded then returns defaults") {
    const auto cfg = AppConfig::load("/nonexistent/path/config.ini");

    REQUIRE(cfg.columns   == kDefaults.columns);
    REQUIRE(cfg.rows      == kDefaults.rows);
    REQUIRE(cfg.fontSize  == kDefaults.fontSize);
    REQUIRE(cfg.textColour == kDefaults.textColour);
    REQUIRE(cfg.bgColour   == kDefaults.bgColour);
}

TEST_CASE("given valid ini when AppConfig loaded then all values are read") {
    const TempIni ini{
        "[Panel]\n"
        "Columns=100\nRows=40\nFontSize=14\n"
        "[Colors]\n"
        "TextR=255\nTextG=128\nTextB=0\n"
        "BgR=20\nBgG=30\nBgB=40\n"
    };

    const auto cfg = AppConfig::load(ini.wxPath());

    REQUIRE(cfg.columns   == 100);
    REQUIRE(cfg.rows      == 40);
    REQUIRE(cfg.fontSize  == 14);
    REQUIRE(cfg.textColour == wxColour(255, 128, 0));
    REQUIRE(cfg.bgColour   == wxColour(20, 30, 40));
}

TEST_CASE("given partial ini when AppConfig loaded then unspecified values remain defaults") {
    const TempIni ini{"[Panel]\nColumns=132\n"};

    const auto cfg = AppConfig::load(ini.wxPath());

    REQUIRE(cfg.columns  == 132);
    REQUIRE(cfg.rows     == kDefaults.rows);
    REQUIRE(cfg.fontSize == kDefaults.fontSize);
    REQUIRE(cfg.bgColour  == kDefaults.bgColour);
}

TEST_CASE("given empty ini when AppConfig loaded then returns defaults") {
    const TempIni ini{""};

    const auto cfg = AppConfig::load(ini.wxPath());

    REQUIRE(cfg.columns   == kDefaults.columns);
    REQUIRE(cfg.rows      == kDefaults.rows);
    REQUIRE(cfg.fontSize  == kDefaults.fontSize);
    REQUIRE(cfg.textColour == kDefaults.textColour);
    REQUIRE(cfg.bgColour   == kDefaults.bgColour);
}
