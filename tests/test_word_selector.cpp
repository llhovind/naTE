#include <catch2/catch_test_macros.hpp>
#include "layout/WordSelector.h"

using WordSelector::FindWordBounds;

static const std::string kDefault = "[^\\s]+";

TEST_CASE("given empty line when FindWordBounds then returns zero-length span")
{
    auto [s, e] = FindWordBounds(U"", 0, kDefault);
    CHECK(s == 0);
    CHECK(e == 0);
}

TEST_CASE("given single word when double-click on it then selects whole word")
{
    const std::u32string line = U"hello";
    auto [s, e] = FindWordBounds(line, 2, kDefault);
    CHECK(s == 0);
    CHECK(e == 5);
}

TEST_CASE("given two words when double-click on first then selects first word")
{
    const std::u32string line = U"hello world";
    auto [s, e] = FindWordBounds(line, 1, kDefault);
    CHECK(s == 0);
    CHECK(e == 5);
}

TEST_CASE("given two words when double-click on second then selects second word")
{
    const std::u32string line = U"hello world";
    auto [s, e] = FindWordBounds(line, 7, kDefault);
    CHECK(s == 6);
    CHECK(e == 11);
}

TEST_CASE("given click on whitespace when FindWordBounds then returns zero-length span")
{
    const std::u32string line = U"hello world";
    auto [s, e] = FindWordBounds(line, 5, kDefault);
    CHECK(s == e);
}

TEST_CASE("given path-like token when double-click then selects whole path")
{
    const std::u32string line = U"see /home/user/file.txt here";
    // Default [^\s]+ matches the whole /home/user/file.txt token.
    auto [s, e] = FindWordBounds(line, 10, kDefault);
    CHECK(s == 4);
    CHECK(e == 23);
}

TEST_CASE("given custom alphanumeric regex when double-click on dotted name then splits at dot")
{
    const std::string alphaNumRegex = "[a-zA-Z0-9_]+";
    const std::u32string line = U"file.txt";
    // With [a-zA-Z0-9_]+, "file" and "txt" are separate tokens.
    auto [s, e] = FindWordBounds(line, 0, alphaNumRegex);
    CHECK(s == 0);
    CHECK(e == 4);
}

TEST_CASE("given custom path regex when double-click then selects filename with extension")
{
    const std::string pathRegex = "[a-zA-Z0-9_.-]+";
    const std::u32string line = U"load file.txt now";
    auto [s, e] = FindWordBounds(line, 6, pathRegex);
    CHECK(s == 5);
    CHECK(e == 13);
}

TEST_CASE("given invalid regex when FindWordBounds then falls back without crashing")
{
    const std::u32string line = U"hello world";
    // Should not throw; should fall back to "[^\\s]+" and return a valid span.
    REQUIRE_NOTHROW(FindWordBounds(line, 2, "[invalid("));
    auto [s, e] = FindWordBounds(line, 2, "[invalid(");
    // Fallback "[^\\s]+" selects "hello".
    CHECK(s == 0);
    CHECK(e == 5);
}

TEST_CASE("given click col beyond line length when FindWordBounds then returns zero-length span")
{
    const std::u32string line = U"hi";
    auto [s, e] = FindWordBounds(line, 10, kDefault);
    CHECK(s == e);
}

TEST_CASE("given line with multiple spaces when double-click on middle word then selects correct token")
{
    // "one   two   three"
    //  0123456789...
    // "two" occupies cols 6-8; click at col 7 (inside "two").
    const std::u32string line = U"one   two   three";
    auto [s, e] = FindWordBounds(line, 7, kDefault);
    CHECK(s == 6);
    CHECK(e == 9);
}
