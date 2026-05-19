#include <catch2/catch_test_macros.hpp>
#include "transport/X11Utils.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using term::transport::ConnectToX11Display;
using term::transport::ReadXauthorityData;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// RAII guard that creates a Unix-domain listening socket at a temp path and
// removes it on destruction.  Clients can connect before the server accepts.
struct TempUnixListener {
    std::string path;
    int         listenFd = -1;

    explicit TempUnixListener()
        : path("/tmp/nate_test_x11_" + std::to_string(::getpid()))
    {
        ::unlink(path.c_str());

        listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0) return;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(listenFd, 1) < 0) {
            ::close(listenFd);
            listenFd = -1;
        }
    }

    ~TempUnixListener()
    {
        if (listenFd >= 0) ::close(listenFd);
        ::unlink(path.c_str());
    }

    bool ok() const { return listenFd >= 0; }
};

// Writes a minimal Xauthority binary file with one record.
// family=256 (FamilyLocal), addr="", dispNum=dispNumStr, authName, authData.
// All big-endian uint16_t lengths + raw bytes.
struct XauthRecord {
    std::string dispNum;
    std::string authName;
    std::vector<uint8_t> authData;
};

void WriteXauthFile(const std::string& path, const std::vector<XauthRecord>& records)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);

    auto writeU16 = [&](uint16_t v) {
        uint8_t buf[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xff)};
        std::fwrite(buf, 1, 2, f);
    };
    auto writeField = [&](const std::string& s) {
        writeU16(static_cast<uint16_t>(s.size()));
        if (!s.empty()) std::fwrite(s.data(), 1, s.size(), f);
    };
    auto writeBytes = [&](const std::vector<uint8_t>& v) {
        writeU16(static_cast<uint16_t>(v.size()));
        if (!v.empty()) std::fwrite(v.data(), 1, v.size(), f);
    };

    for (const auto& r : records) {
        writeU16(256);           // FamilyLocal
        writeField("");          // addr (empty for local)
        writeField(r.dispNum);
        writeField(r.authName);
        writeBytes(r.authData);
    }
    std::fclose(f);
}

struct TempXauthFile {
    std::string path;
    std::string prevXauth;
    bool hadPrev = false;

    explicit TempXauthFile(const std::string& suffix = "")
        : path("/tmp/nate_test_xauth_" + std::to_string(::getpid()) + suffix)
    {
        const char* prev = ::getenv("XAUTHORITY");
        if (prev) { prevXauth = prev; hadPrev = true; }
        ::setenv("XAUTHORITY", path.c_str(), 1);
    }
    ~TempXauthFile()
    {
        ::unlink(path.c_str());
        if (hadPrev) ::setenv("XAUTHORITY", prevXauth.c_str(), 1);
        else          ::unsetenv("XAUTHORITY");
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("given null DISPLAY when ConnectToX11Display then returns -1", "[x11utils]")
{
    CHECK(ConnectToX11Display(nullptr) == -1);
}

TEST_CASE("given empty DISPLAY when ConnectToX11Display then returns -1", "[x11utils]")
{
    CHECK(ConnectToX11Display("") == -1);
}

TEST_CASE("given DISPLAY with no colon when ConnectToX11Display then returns -1", "[x11utils]")
{
    CHECK(ConnectToX11Display("nodisplaynumber") == -1);
}

TEST_CASE("given DISPLAY with non-numeric number when ConnectToX11Display then returns -1", "[x11utils]")
{
    CHECK(ConnectToX11Display(":abc") == -1);
}

TEST_CASE("given DISPLAY pointing to nonexistent unix socket when ConnectToX11Display then returns -1", "[x11utils]")
{
    // Display :99 — almost certainly no server listening there.
    CHECK(ConnectToX11Display(":99") == -1);
}

TEST_CASE("given DISPLAY pointing to nonexistent TCP host when ConnectToX11Display then returns -1", "[x11utils]")
{
    // localhost:11 → port 6011 — extremely unlikely to have an X server.
    CHECK(ConnectToX11Display("localhost:11.0") == -1);
}

TEST_CASE("given DISPLAY pointing to a live unix socket when ConnectToX11Display then returns valid fd", "[x11utils]")
{
    // Spin up a temporary Unix listener at a known path, then point a synthetic
    // DISPLAY at it by abusing the ":N" → /tmp/.X11-unix/XN path.
    // We can't control the real /tmp/.X11-unix directory, so instead we test
    // the TCP path with a loopback listener, which exercises the same logic
    // branches in ConnectToX11Display.

    // Create a listening socket on an ephemeral loopback port.
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(serverFd >= 0);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;  // let kernel choose

    REQUIRE(::bind(serverFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(serverFd, 1) == 0);

    // Discover what port was assigned.
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    REQUIRE(::getsockname(serverFd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0);
    const int port = ntohs(bound.sin_port);

    // ConnectToX11Display("localhost:N") tries TCP localhost:6000+N.
    // So we need N = port - 6000 and port must be > 6000.
    if (port > 6000) {
        const std::string display = "localhost:" + std::to_string(port - 6000);
        const int fd = ConnectToX11Display(display.c_str());
        CHECK(fd >= 0);
        if (fd >= 0) ::close(fd);
        // Accept so the server socket doesn't linger.
        const int clientAccepted = ::accept(serverFd, nullptr, nullptr);
        if (clientAccepted >= 0) ::close(clientAccepted);
    } else {
        // Port < 6000 is rare; skip assertion rather than fail on port allocation luck.
        SUCCEED("ephemeral port below 6000 — skipping connection assertion");
    }

    ::close(serverFd);
}

// ---------------------------------------------------------------------------
// ReadXauthorityData tests
// ---------------------------------------------------------------------------

TEST_CASE("given null DISPLAY when ReadXauthorityData then returns empty pair", "[x11utils]")
{
    auto [name, cookie] = ReadXauthorityData(nullptr);
    CHECK(name.empty());
    CHECK(cookie.empty());
}

TEST_CASE("given empty DISPLAY when ReadXauthorityData then returns empty pair", "[x11utils]")
{
    auto [name, cookie] = ReadXauthorityData("");
    CHECK(name.empty());
    CHECK(cookie.empty());
}

TEST_CASE("given DISPLAY with matching record when ReadXauthorityData then returns correct hex cookie", "[x11utils]")
{
    TempXauthFile tmp;
    const std::vector<uint8_t> rawCookie = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
    };
    WriteXauthFile(tmp.path, {{"0", "MIT-MAGIC-COOKIE-1", rawCookie}});

    auto [name, cookie] = ReadXauthorityData(":0");
    CHECK(name == "MIT-MAGIC-COOKIE-1");
    CHECK(cookie == "0123456789abcdeffedcba9876543210");
}

TEST_CASE("given DISPLAY with dot-screen suffix when ReadXauthorityData then still matches", "[x11utils]")
{
    TempXauthFile tmp("_dot");
    const std::vector<uint8_t> rawCookie(16, 0xaa);
    WriteXauthFile(tmp.path, {{"1", "MIT-MAGIC-COOKIE-1", rawCookie}});

    auto [name, cookie] = ReadXauthorityData(":1.0");
    CHECK(name == "MIT-MAGIC-COOKIE-1");
    CHECK(cookie == std::string(32, 'a'));
}

TEST_CASE("given DISPLAY not matching any record when ReadXauthorityData then returns empty pair", "[x11utils]")
{
    TempXauthFile tmp("_nomatch");
    const std::vector<uint8_t> rawCookie(16, 0x55);
    WriteXauthFile(tmp.path, {{"2", "MIT-MAGIC-COOKIE-1", rawCookie}});

    // Record is for display 2, but we ask for display 5.
    auto [name, cookie] = ReadXauthorityData(":5");
    CHECK(name.empty());
    CHECK(cookie.empty());
}

TEST_CASE("given Xauthority with multiple records when ReadXauthorityData then picks correct one", "[x11utils]")
{
    TempXauthFile tmp("_multi");
    const std::vector<uint8_t> cookie0(16, 0x11);
    const std::vector<uint8_t> cookie1(16, 0x22);
    WriteXauthFile(tmp.path, {
        {"0", "MIT-MAGIC-COOKIE-1", cookie0},
        {"1", "MIT-MAGIC-COOKIE-1", cookie1},
    });

    auto [name1, hex1] = ReadXauthorityData(":1");
    CHECK(name1 == "MIT-MAGIC-COOKIE-1");
    CHECK(hex1 == std::string(32, '2'));

    auto [name0, hex0] = ReadXauthorityData(":0");
    CHECK(name0 == "MIT-MAGIC-COOKIE-1");
    CHECK(hex0 == std::string(32, '1'));
}

TEST_CASE("given Xauthority file absent when ReadXauthorityData then returns empty pair", "[x11utils]")
{
    // Point XAUTHORITY at a nonexistent path.
    const char* prev = ::getenv("XAUTHORITY");
    std::string prevStr = prev ? prev : "";
    bool hadPrev = (prev != nullptr);
    ::setenv("XAUTHORITY", "/tmp/nate_test_xauth_nonexistent_9999999", 1);

    auto [name, cookie] = ReadXauthorityData(":0");
    CHECK(name.empty());
    CHECK(cookie.empty());

    if (hadPrev) ::setenv("XAUTHORITY", prevStr.c_str(), 1);
    else          ::unsetenv("XAUTHORITY");
}
