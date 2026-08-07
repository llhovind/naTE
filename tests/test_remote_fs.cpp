#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "session/Connection.h"
#include "session/SessionManager.h"
#include "transport/IRemoteFileSystem.h"

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
// Callback contract
//
// The port promises every callback fires exactly once, including on the paths
// that cannot do any work. Dropping the callback instead would leave a caller
// waiting forever on a spinner that never resolves — the failure mode these
// tests exist to prevent.
// ---------------------------------------------------------------------------

TEST_CASE("given a session with no filesystem when a listing is requested then it reports not connected") {
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    std::optional<FsError> result;
    std::vector<FileInfo>  entries{FileInfo{}};  // seeded to prove it is cleared

    sm.ListRemoteDirectory(id, "/etc",
        [&](std::vector<FileInfo> es, FsError err) {
            entries = std::move(es);
            result  = std::move(err);
        });

    REQUIRE(result.has_value());
    REQUIRE(result->code == FsErrorCode::NotConnected);
    REQUIRE_FALSE(result->message.empty());
    REQUIRE(entries.empty());

    sm.CloseSession(id);
}

TEST_CASE("given a session with no filesystem when a transfer is requested then it reports not connected") {
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    SECTION("download") {
        std::optional<FsError> result;
        sm.DownloadFile(id, "/etc/hosts", "/tmp/hosts",
                        [&](FsError err) { result = std::move(err); });
        REQUIRE(result.has_value());
        REQUIRE(result->code == FsErrorCode::NotConnected);
    }

    SECTION("upload") {
        std::optional<FsError> result;
        sm.UploadFile(id, "/tmp/hosts", "/etc/hosts",
                      [&](FsError err) { result = std::move(err); });
        REQUIRE(result.has_value());
        REQUIRE(result->code == FsErrorCode::NotConnected);
    }

    SECTION("send into a directory") {
        std::optional<FsError> result;
        sm.SendFile(id, "/tmp/hosts", "/etc",
                    [&](FsError err) { result = std::move(err); });
        REQUIRE(result.has_value());
        REQUIRE(result->code == FsErrorCode::NotConnected);
    }

    SECTION("receive into a directory") {
        std::optional<FsError> result;
        sm.ReceiveFile(id, "/etc/hosts", "/tmp",
                       [&](FsError err) { result = std::move(err); });
        REQUIRE(result.has_value());
        REQUIRE(result->code == FsErrorCode::NotConnected);
    }

    sm.CloseSession(id);
}

TEST_CASE("given an unknown session when a transfer between sessions is requested then it reports not connected") {
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    SECTION("local to remote") {
        std::optional<FsError> result;
        sm.TransferFileBetweenSessions(0, "/tmp/a.txt", id, "/var/tmp",
                                       [&](FsError err) { result = std::move(err); });
        REQUIRE(result.has_value());
        REQUIRE(result->code == FsErrorCode::NotConnected);
    }

    SECTION("remote to local") {
        std::optional<FsError> result;
        sm.TransferFileBetweenSessions(id, "/var/tmp/a.txt", 0, "/tmp",
                                       [&](FsError err) { result = std::move(err); });
        REQUIRE(result.has_value());
        REQUIRE(result->code == FsErrorCode::NotConnected);
    }

    sm.CloseSession(id);
}

TEST_CASE("given a failing remote to remote transfer when the source download fails then the callback still fires once") {
    SessionManager sm;
    const SessionId src = sm.CreateSession(loopbackConn("src"), 1000, 80, 24);
    const SessionId dst = sm.CreateSession(loopbackConn("dst"), 1000, 80, 24);

    int  calls = 0;
    std::optional<FsError> result;
    sm.TransferFileBetweenSessions(src, "/etc/hosts", dst, "/tmp",
                                   [&](FsError err) {
                                       ++calls;
                                       result = std::move(err);
                                   });

    // The temp directory is created, the download fails immediately because the
    // source has no filesystem, and the error propagates without the upload
    // leg ever running.
    REQUIRE(calls == 1);
    REQUIRE(result.has_value());
    REQUIRE(result->code == FsErrorCode::NotConnected);

    sm.CloseSession(src);
    sm.CloseSession(dst);
}
