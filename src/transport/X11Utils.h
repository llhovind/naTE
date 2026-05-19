#pragma once

#include <string>
#include <utility>

namespace term::transport {

// Opens a socket connected to the local X11 server described by the display
// string (the value of $DISPLAY).  Returns the socket fd on success, or -1 if
// the display is absent, malformed, or unreachable.
//
// Supported formats:
//   :N          — Unix domain socket /tmp/.X11-unix/XN
//   unix:N      — same
//   host:N[.s]  — TCP host:6000+N
int ConnectToX11Display(const char* display);

// Reads the MIT-MAGIC-COOKIE-1 entry from the Xauthority file
// ($XAUTHORITY or ~/.Xauthority) that matches the given $DISPLAY string.
//
// Returns {auth_proto_name, hex_cookie} on success, or {"", ""} if not found.
// hex_cookie is a lowercase hex string (32 chars for the standard 16-byte cookie),
// exactly as expected by libssh2_channel_x11_req_ex().
std::pair<std::string, std::string> ReadXauthorityData(const char* display);

} // namespace term::transport
