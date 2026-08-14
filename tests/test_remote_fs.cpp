#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "session/Connection.h"
#include "session/SessionManager.h"
#include "transport/IRemoteFileSystem.h"
#include "transport/LocalFileSystem.h"

using namespace term::session;
using namespace term::transport;

namespace {

Connection loopbackConn(const std::string& label = "Test") {
    return Connection{label, LoopbackDesc{}};
}

} // namespace

// ---------------------------------------------------------------------------
// Capability
// ---------------------------------------------------------------------------

TEST_CASE("given a transport without a filesystem when queried then the port is null") {
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    REQUIRE(sm.GetRemoteFileSystem(id) == nullptr);
    REQUIRE_FALSE(sm.SupportsFileTransfer(id));

    sm.CloseSession(id);
}

TEST_CASE("given an unknown session id when queried then the port is null") {
    SessionManager sm;
    REQUIRE(sm.GetRemoteFileSystem(9999) == nullptr);
    REQUIRE_FALSE(sm.SupportsFileTransfer(9999));
}

// ---------------------------------------------------------------------------
// Reverse lookup
//
// fs/ works in terms of the port and holds no session id, so the way back —
// from a filesystem to the session that owns it — has to be reliable, and in
// particular has to answer "nobody" rather than guess.
// ---------------------------------------------------------------------------

TEST_CASE("given a filesystem no session owns when looked up then it names no session") {
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    SECTION("a filesystem belonging to nothing") {
        LocalFileSystem unowned;
        REQUIRE(sm.FindSessionForFileSystem(&unowned) == 0);
    }

    SECTION("no filesystem at all") {
        REQUIRE(sm.FindSessionForFileSystem(nullptr) == 0);
    }

    sm.CloseSession(id);
}
