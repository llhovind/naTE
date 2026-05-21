#include <catch2/catch_test_macros.hpp>
#include "ui/UrlScanner.h"

using UrlScanner::ScanLine;
using UrlScanner::UrlSpan;

TEST_CASE("given empty string when ScanLine then returns no spans")
{
    REQUIRE(ScanLine(U"").empty());
}

TEST_CASE("given plain text with no url when ScanLine then returns no spans")
{
    REQUIRE(ScanLine(U"hello world").empty());
}

TEST_CASE("given https url when ScanLine then returns one span with correct col and url")
{
    const auto spans = ScanLine(U"https://example.com");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].col == 0);
    CHECK(spans[0].url == U"https://example.com");
    CHECK(spans[0].len == 19);
}

TEST_CASE("given http url mid-line when ScanLine then col reflects offset")
{
    const auto spans = ScanLine(U"Visit http://example.com today");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].col == 6);
    CHECK(spans[0].url == U"http://example.com");
}

TEST_CASE("given url with trailing period when ScanLine then period is trimmed")
{
    const auto spans = ScanLine(U"See https://example.com.");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].url == U"https://example.com");
}

TEST_CASE("given url with trailing comma when ScanLine then comma is trimmed")
{
    const auto spans = ScanLine(U"See https://example.com, for more");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].url == U"https://example.com");
}

TEST_CASE("given url with trailing closing paren when ScanLine then paren is trimmed")
{
    const auto spans = ScanLine(U"(see https://example.com)");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].url == U"https://example.com");
}

TEST_CASE("given two urls on one line when ScanLine then returns two spans in order")
{
    const auto spans = ScanLine(U"https://a.com and https://b.com");
    REQUIRE(spans.size() == 2);
    CHECK(spans[0].url == U"https://a.com");
    CHECK(spans[1].url == U"https://b.com");
    CHECK(spans[0].col < spans[1].col);
}

TEST_CASE("given ftp url when ScanLine then recognized")
{
    const auto spans = ScanLine(U"ftp://files.example.com/path");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].url == U"ftp://files.example.com/path");
}

TEST_CASE("given mailto url when ScanLine then recognized")
{
    const auto spans = ScanLine(U"Contact mailto:user@example.com");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].url == U"mailto:user@example.com");
}

TEST_CASE("given url with path and query when ScanLine then full url captured")
{
    const auto spans = ScanLine(U"https://example.com/path?q=1&r=2#anchor");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].url == U"https://example.com/path?q=1&r=2#anchor");
}

TEST_CASE("given https url adjacent to text when ScanLine then correct boundaries")
{
    // URL immediately preceded by non-space text via colon (not a scheme)
    const auto spans = ScanLine(U"URL:https://example.com end");
    REQUIRE(spans.size() == 1);
    // 'URL:' is not a recognised scheme; scan starts at 'https://'
    CHECK(spans[0].url == U"https://example.com");
}

TEST_CASE("given url with uppercase scheme when ScanLine then recognized case-insensitively")
{
    const auto spans = ScanLine(U"HTTPS://example.com");
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].col == 0);
    // The captured url preserves original case
    CHECK(spans[0].url == U"HTTPS://example.com");
}
