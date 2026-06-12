#include <catch2/catch_test_macros.hpp>
#include "transport/EnvUtils.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace term::transport;
using term::transport::EnvVar;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

struct TempEnvFile {
    std::filesystem::path path;

    explicit TempEnvFile(const std::string& contents)
        : path(std::filesystem::temp_directory_path() / "nate_test_envfile.env")
    {
        std::ofstream f(path);
        f << contents;
    }
    ~TempEnvFile() { std::filesystem::remove(path); }

    std::string str() const { return path.string(); }
};

// Build a block and collect it back into a map for easy assertions.
std::unordered_map<std::string, std::string> BlockToMap(const EnvBlock& block)
{
    std::unordered_map<std::string, std::string> m;
    for (char* p : block.ptrs) {
        if (!p) break;
        std::string entry(p);
        const auto eq = entry.find('=');
        if (eq != std::string::npos)
            m[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// ParseEnvFile
// ---------------------------------------------------------------------------

TEST_CASE("given valid .env file when parsed then returns correct EnvVar list")
{
    TempEnvFile f("KEY1=value1\nKEY2=value2\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 2);
    CHECK(vars[0].key   == "KEY1");
    CHECK(vars[0].value == "value1");
    CHECK(vars[1].key   == "KEY2");
    CHECK(vars[1].value == "value2");
}

TEST_CASE("given .env with comment lines when parsed then comments are skipped")
{
    TempEnvFile f("# this is a comment\nKEY=val\n# another\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].key == "KEY");
}

TEST_CASE("given .env with blank lines when parsed then blank lines are skipped")
{
    TempEnvFile f("\n\nKEY=val\n\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].key == "KEY");
}

TEST_CASE("given .env with malformed line (no equals) when parsed then line is skipped")
{
    TempEnvFile f("BADLINE\nKEY=val\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].key == "KEY");
}

TEST_CASE("given .env with empty key when parsed then line is skipped")
{
    TempEnvFile f("=value\nKEY=val\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].key == "KEY");
}

TEST_CASE("given .env with KEY= when parsed then EnvVar has empty value")
{
    TempEnvFile f("KEY=\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].key   == "KEY");
    CHECK(vars[0].value == "");
}

TEST_CASE("given .env with value containing equals when parsed then value includes everything after first equals")
{
    TempEnvFile f("KEY=a=b=c\n");
    auto vars = ParseEnvFile(f.str());
    REQUIRE(vars.size() == 1);
    CHECK(vars[0].key   == "KEY");
    CHECK(vars[0].value == "a=b=c");
}

TEST_CASE("given nonexistent file path when ParseEnvFile called then returns empty vector")
{
    auto vars = ParseEnvFile("/nonexistent/path/that/does/not/exist.env");
    CHECK(vars.empty());
}

// ---------------------------------------------------------------------------
// ExpandTilde
// ---------------------------------------------------------------------------

TEST_CASE("given path starting with tilde-slash when ExpandTilde called then tilde replaced with homeDir")
{
    CHECK(ExpandTilde("~/foo/bar", "/home/user") == "/home/user/foo/bar");
}

TEST_CASE("given absolute path when ExpandTilde called then path returned unchanged")
{
    CHECK(ExpandTilde("/abs/path", "/home/user") == "/abs/path");
}

TEST_CASE("given tilde alone when ExpandTilde called then returns homeDir")
{
    CHECK(ExpandTilde("~", "/home/user") == "/home/user");
}

TEST_CASE("given empty path when ExpandTilde called then returns empty string")
{
    CHECK(ExpandTilde("", "/home/user") == "");
}

TEST_CASE("given path starting with tilde-non-slash when ExpandTilde called then path returned unchanged")
{
    // "~other" is not a home-dir reference
    CHECK(ExpandTilde("~other/path", "/home/user") == "~other/path");
}

// ---------------------------------------------------------------------------
// BuildEnvBlock
// ---------------------------------------------------------------------------

TEST_CASE("given parent env A=1 and profile override A=2 when BuildEnvBlock called then result contains A=2")
{
    auto block = BuildEnvBlock({"A=1"}, {}, {}, {{"A", "2"}});
    auto m = BlockToMap(block);
    CHECK(m.at("A") == "2");
}

TEST_CASE("given env file A=file and profile A=profile when BuildEnvBlock called then A=profile")
{
    auto block = BuildEnvBlock({}, {}, {{"A", "file"}}, {{"A", "profile"}});
    auto m = BlockToMap(block);
    CHECK(m.at("A") == "profile");
}

TEST_CASE("given app default A=app and env file A=file when BuildEnvBlock called then A=file")
{
    auto block = BuildEnvBlock({}, {{"A", "app"}}, {{"A", "file"}}, {});
    auto m = BlockToMap(block);
    CHECK(m.at("A") == "file");
}

TEST_CASE("given all four layers setting A when BuildEnvBlock called then profile var wins")
{
    auto block = BuildEnvBlock({"A=parent"}, {{"A", "app"}}, {{"A", "file"}}, {{"A", "profile"}});
    auto m = BlockToMap(block);
    CHECK(m.at("A") == "profile");
}

TEST_CASE("given parent with B=1 and profile adds A=2 when BuildEnvBlock called then both present")
{
    auto block = BuildEnvBlock({"B=1"}, {}, {}, {{"A", "2"}});
    auto m = BlockToMap(block);
    CHECK(m.at("A") == "2");
    CHECK(m.at("B") == "1");
}

TEST_CASE("given empty all layers when BuildEnvBlock called then only nullptr sentinel present")
{
    auto block = BuildEnvBlock({}, {}, {}, {});
    REQUIRE(block.ptrs.size() == 1);
    CHECK(block.ptrs[0] == nullptr);
}

TEST_CASE("given BuildEnvBlock result when ptrs last element is nullptr")
{
    auto block = BuildEnvBlock({"X=1"}, {{"Y", "2"}}, {}, {});
    CHECK(block.ptrs.back() == nullptr);
}

// ---------------------------------------------------------------------------
// ResolveWorkingDir
// ---------------------------------------------------------------------------

TEST_CASE("given non-empty profile dir when ResolveWorkingDir called then profile dir returned tilde-expanded")
{
    CHECK(ResolveWorkingDir("~/work", "/default", "/home/user") == "/home/user/work");
}

TEST_CASE("given empty profile dir and non-empty app default when ResolveWorkingDir called then app default returned")
{
    CHECK(ResolveWorkingDir("", "~/default", "/home/user") == "/home/user/default");
}

TEST_CASE("given both dirs empty when ResolveWorkingDir called then empty string returned")
{
    CHECK(ResolveWorkingDir("", "", "/home/user") == "");
}

TEST_CASE("given absolute profile dir when ResolveWorkingDir called then returned unchanged")
{
    CHECK(ResolveWorkingDir("/opt/project", "", "/home/user") == "/opt/project");
}

// ---------------------------------------------------------------------------
// MakeLoginShellArg0
// ---------------------------------------------------------------------------

TEST_CASE("given /bin/bash when MakeLoginShellArg0 called then returns -bash")
{
    CHECK(MakeLoginShellArg0("/bin/bash") == "-bash");
}

TEST_CASE("given /usr/bin/zsh when MakeLoginShellArg0 called then returns -zsh")
{
    CHECK(MakeLoginShellArg0("/usr/bin/zsh") == "-zsh");
}

TEST_CASE("given /bin/sh when MakeLoginShellArg0 called then returns -sh")
{
    CHECK(MakeLoginShellArg0("/bin/sh") == "-sh");
}

TEST_CASE("given shell name with no path separator when MakeLoginShellArg0 called then returns -name")
{
    CHECK(MakeLoginShellArg0("fish") == "-fish");
}

// ---------------------------------------------------------------------------
// ShellQuote
// ---------------------------------------------------------------------------

TEST_CASE("given plain string when ShellQuote called then wrapped in single quotes")
{
    CHECK(ShellQuote("hello") == "'hello'");
}

TEST_CASE("given string with spaces when ShellQuote called then preserved inside quotes")
{
    CHECK(ShellQuote("/tmp/my file.txt") == "'/tmp/my file.txt'");
}

TEST_CASE("given embedded single quote when ShellQuote called then escaped as quote-backslash-quote")
{
    CHECK(ShellQuote("it's") == "'it'\\''s'");
}

TEST_CASE("given empty string when ShellQuote called then returns empty quoted token")
{
    CHECK(ShellQuote("") == "''");
}

TEST_CASE("given shell metacharacters when ShellQuote called then left verbatim inside quotes")
{
    CHECK(ShellQuote("$HOME; rm -rf `x` && |") == "'$HOME; rm -rf `x` && |'");
}
