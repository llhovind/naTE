#include "transport/X11Utils.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace term::transport {

int ConnectToX11Display(const char* display)
{
    if (!display || display[0] == '\0') return -1;

    const std::string disp(display);

    // Split at the last ':' to handle IPv6 host addresses.
    const size_t colon = disp.rfind(':');
    if (colon == std::string::npos) return -1;

    const std::string host = disp.substr(0, colon);
    const std::string rest = disp.substr(colon + 1);

    int displayNum = 0;
    try { displayNum = std::stoi(rest); }
    catch (...) { return -1; }

    if (host.empty() || host == "unix") {
        // Unix domain socket: /tmp/.X11-unix/XN
        const std::string sockPath = "/tmp/.X11-unix/X" + std::to_string(displayNum);

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    // TCP socket: host:6000+displayNum
    const std::string portStr = std::to_string(6000 + displayNum);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (const addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0)
            break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

// ---------------------------------------------------------------------------
// ReadXauthorityData
// ---------------------------------------------------------------------------

namespace {

// Read a big-endian uint16_t from the file; returns false on EOF/error.
bool ReadU16(std::FILE* f, uint16_t& out)
{
    uint8_t buf[2];
    if (std::fread(buf, 1, 2, f) != 2) return false;
    out = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    return true;
}

// Read a length-prefixed byte string (uint16_t count + bytes).
bool ReadField(std::FILE* f, std::string& out)
{
    uint16_t len = 0;
    if (!ReadU16(f, len)) return false;
    out.resize(len);
    if (len > 0 && std::fread(out.data(), 1, len, f) != len) return false;
    return true;
}

// Extract the display number from a $DISPLAY string.
// ":N[.s]", "unix:N[.s]", "host:N[.s]"  → N as string, e.g. "0"
// Returns "" on failure.
std::string DisplayNumStr(const char* display)
{
    if (!display || display[0] == '\0') return {};
    const std::string disp(display);

    const size_t colon = disp.rfind(':');
    if (colon == std::string::npos) return {};

    std::string numAndScreen = disp.substr(colon + 1);
    const size_t dot = numAndScreen.find('.');
    if (dot != std::string::npos) numAndScreen.erase(dot);

    if (numAndScreen.empty()) return {};
    for (char c : numAndScreen)
        if (c < '0' || c > '9') return {};
    return numAndScreen;
}

// Locate the Xauthority file: $XAUTHORITY, then ~/.Xauthority.
std::string XauthorityPath()
{
    const char* env = ::getenv("XAUTHORITY");
    if (env && env[0] != '\0') return env;

    const char* home = ::getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd* pw = ::getpwuid(::getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home || home[0] == '\0') return {};
    return std::string(home) + "/.Xauthority";
}

} // anonymous namespace

std::pair<std::string, std::string> ReadXauthorityData(const char* display)
{
    const std::string targetNum = DisplayNumStr(display);
    if (targetNum.empty()) return {};

    const std::string path = XauthorityPath();
    if (path.empty()) return {};

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};

    std::pair<std::string, std::string> result;

    // Each record: family(u16) | addr | display_num | auth_name | auth_data
    while (!std::feof(f)) {
        uint16_t family = 0;
        if (!ReadU16(f, family)) break;

        std::string addr, dispNum, authName, authData;
        if (!ReadField(f, addr))     break;
        if (!ReadField(f, dispNum))  break;
        if (!ReadField(f, authName)) break;
        if (!ReadField(f, authData)) break;

        if (dispNum != targetNum) continue;
        if (authName != "MIT-MAGIC-COOKIE-1") continue;

        // Convert raw bytes to lowercase hex string.
        std::string hex;
        hex.reserve(authData.size() * 2);
        static constexpr char kHex[] = "0123456789abcdef";
        for (unsigned char byte : authData) {
            hex += kHex[byte >> 4];
            hex += kHex[byte & 0x0f];
        }
        result = {authName, hex};
        break;
    }

    std::fclose(f);
    return result;
}

} // namespace term::transport
