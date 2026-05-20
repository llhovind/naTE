#include <catch2/catch_test_macros.hpp>
#include "db/ConnectionSerialisation.h"
#include "session/Connection.h"

using term::session::SshAuthMethod;
using term::db::serialisation::AuthMethodToString;
using term::db::serialisation::AuthMethodFromString;

TEST_CASE("given KbdInteractive when AuthMethodToString then returns keyboard-interactive") {
    CHECK(AuthMethodToString(SshAuthMethod::KbdInteractive) == "keyboard-interactive");
}

TEST_CASE("given keyboard-interactive string when AuthMethodFromString then returns KbdInteractive") {
    CHECK(AuthMethodFromString("keyboard-interactive") == SshAuthMethod::KbdInteractive);
}

TEST_CASE("given unknown string when AuthMethodFromString then returns Agent") {
    CHECK(AuthMethodFromString("unknown") == SshAuthMethod::Agent);
    CHECK(AuthMethodFromString("")        == SshAuthMethod::Agent);
}

TEST_CASE("given all auth methods when AuthMethodToString then each round-trips") {
    for (auto m : { SshAuthMethod::Agent, SshAuthMethod::Password,
                    SshAuthMethod::PrivateKey, SshAuthMethod::KbdInteractive }) {
        CHECK(AuthMethodFromString(AuthMethodToString(m)) == m);
    }
}

TEST_CASE("given SshDesc with KbdInteractive when serialised and deserialised then authMethod preserved") {
    term::session::SshDesc original;
    original.host              = "myhost.example.com";
    original.port              = 2222;
    original.username          = "alice";
    original.authMethod        = SshAuthMethod::KbdInteractive;
    original.keepaliveSeconds  = 60;
    original.connectTimeoutSec = 15;

    const auto j = term::db::serialisation::SerialiseTransport(original);
    REQUIRE(j.at("type").get<std::string>() == "ssh");
    REQUIRE(j.at("authMethod").get<std::string>() == "keyboard-interactive");

    const auto desc = std::get<term::session::SshDesc>(
        term::db::serialisation::DeserialiseTransport(j));

    CHECK(desc.host              == original.host);
    CHECK(desc.port              == original.port);
    CHECK(desc.username          == original.username);
    CHECK(desc.authMethod        == SshAuthMethod::KbdInteractive);
    CHECK(desc.keepaliveSeconds  == original.keepaliveSeconds);
    CHECK(desc.connectTimeoutSec == original.connectTimeoutSec);
}
