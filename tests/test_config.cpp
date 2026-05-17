#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "config/Config.h"

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

    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.columns   == 100);
    REQUIRE(cfg.rows      == 40);
    REQUIRE(cfg.fontSize  == 14);
    REQUIRE(cfg.textColour == (Rgb{255, 128, 0}));
    REQUIRE(cfg.bgColour   == (Rgb{20, 30, 40}));
}

TEST_CASE("given partial ini when AppConfig loaded then unspecified values remain defaults") {
    const TempIni ini{"[Panel]\nColumns=132\n"};

    const auto cfg = AppConfig::load(ini.stdPath());

    REQUIRE(cfg.columns  == 132);
    REQUIRE(cfg.rows     == kDefaults.rows);
    REQUIRE(cfg.fontSize == kDefaults.fontSize);
    REQUIRE(cfg.bgColour  == kDefaults.bgColour);
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
