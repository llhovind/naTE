#include <catch2/catch_test_macros.hpp>
#include "db/ConnectionSerialisation.h"
#include "session/Connection.h"
#include "transport/PortForward.h"

using namespace term::session;
using namespace term::transport;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static SshDesc makeSshWithForwards()
{
    SshDesc d;
    d.host     = "example.com";
    d.username = "alice";

    PortForwardDesc local;
    local.direction  = PortForwardDirection::Local;
    local.localPort  = 5432;
    local.remoteHost = "db.internal";
    local.remotePort = 5432;
    local.bindAddr   = "127.0.0.1";
    local.label      = "Postgres";
    local.id         = 7;  // should NOT be persisted

    PortForwardDesc remote;
    remote.direction  = PortForwardDirection::Remote;
    remote.localPort  = 8080;
    remote.remoteHost = "localhost";
    remote.remotePort = 9090;
    remote.bindAddr   = "0.0.0.0";
    remote.label      = "Web proxy";
    remote.id         = 42;  // should NOT be persisted

    d.portForwards = { local, remote };
    return d;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("given SshDesc with portForwards when serialised then JSON contains portForwards array")
{
    SshDesc ssh = makeSshWithForwards();
    TransportDesc td = ssh;
    nlohmann::json j = term::db::serialisation::SerialiseTransport(td);

    REQUIRE(j.contains("portForwards"));
    REQUIRE(j["portForwards"].is_array());
    REQUIRE(j["portForwards"].size() == 2);

    // First entry: local
    const auto& pf0 = j["portForwards"][0];
    CHECK(pf0["direction"] == "local");
    CHECK(pf0["localPort"]  == 5432);
    CHECK(pf0["remoteHost"] == "db.internal");
    CHECK(pf0["remotePort"] == 5432);
    CHECK(pf0["bindAddr"]   == "127.0.0.1");
    CHECK(pf0["label"]      == "Postgres");
    CHECK_FALSE(pf0.contains("id"));  // IDs are NOT persisted

    // Second entry: remote
    const auto& pf1 = j["portForwards"][1];
    CHECK(pf1["direction"] == "remote");
}

TEST_CASE("given JSON with portForwards when deserialised then fields survive round-trip")
{
    SshDesc ssh = makeSshWithForwards();
    TransportDesc td = ssh;

    // Round-trip through JSON
    nlohmann::json j  = term::db::serialisation::SerialiseTransport(td);
    TransportDesc td2 = term::db::serialisation::DeserialiseTransport(j);

    REQUIRE(std::holds_alternative<SshDesc>(td2));
    const SshDesc& out = std::get<SshDesc>(td2);

    REQUIRE(out.portForwards.size() == 2);

    SECTION("local forward fields survive")
    {
        const auto& pf = out.portForwards[0];
        CHECK(pf.direction  == PortForwardDirection::Local);
        CHECK(pf.localPort  == 5432);
        CHECK(pf.remoteHost == "db.internal");
        CHECK(pf.remotePort == 5432);
        CHECK(pf.bindAddr   == "127.0.0.1");
        CHECK(pf.label      == "Postgres");
    }

    SECTION("remote forward fields survive")
    {
        const auto& pf = out.portForwards[1];
        CHECK(pf.direction  == PortForwardDirection::Remote);
        CHECK(pf.localPort  == 8080);
        CHECK(pf.remoteHost == "localhost");
        CHECK(pf.remotePort == 9090);
        CHECK(pf.bindAddr   == "0.0.0.0");
        CHECK(pf.label      == "Web proxy");
    }

    SECTION("IDs are NOT persisted in JSON — assigned sequentially on load")
    {
        // IDs are runtime-only; they are stripped on serialization and
        // re-assigned starting at 1 when the JSON is deserialized.
        CHECK(out.portForwards[0].id == 1);
        CHECK(out.portForwards[1].id == 2);
    }
}

TEST_CASE("given SshDesc with no portForwards when serialised then JSON omits portForwards key")
{
    SshDesc ssh;
    ssh.host     = "example.com";
    ssh.username = "bob";
    TransportDesc td = ssh;

    nlohmann::json j = term::db::serialisation::SerialiseTransport(td);
    CHECK_FALSE(j.contains("portForwards"));
}

TEST_CASE("given JSON with missing portForwards when deserialised then portForwards is empty")
{
    nlohmann::json j = {
        {"type",     "ssh"},
        {"host",     "example.com"},
        {"username", "carol"},
        {"port",     22}
    };

    TransportDesc td = term::db::serialisation::DeserialiseTransport(j);
    REQUIRE(std::holds_alternative<SshDesc>(td));
    CHECK(std::get<SshDesc>(td).portForwards.empty());
}
