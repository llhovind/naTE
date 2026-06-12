#include <catch2/catch_test_macros.hpp>
#include "db/JsonSessionRestoreRepository.h"
#include "session/RestoreState.h"
#include "session/Connection.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string TmpPath()
{
    return fs::temp_directory_path() / "nate_restore_test.json";
}

static void Cleanup(const std::string& path)
{
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

static term::session::RestoreState MakeSampleState()
{
    term::session::RestoreState state;

    // Window 1: two tiles
    {
        term::session::RestoreWindow w;
        w.x = 100; w.y = 50; w.width = 1280; w.height = 720;

        // Tile 1: PTY session
        {
            term::session::RestoreTile tile;
            tile.activeTabIndex = 0;
            term::session::Connection c;
            c.label       = "local-shell";
            c.transport   = term::transport::PtyDesc{"/bin/bash"};
            c.wrapMode    = true;
            c.columnWidth = 132;
            c.rows        = 50;
            c.sessionInit.workingDir = "/tmp";
            c.sessionInit.loginShell = true;
            tile.sessions.push_back({c});
            w.tiles.push_back(std::move(tile));
        }

        // Tile 2: SSH + Serial in two tabs (active tab = 1)
        {
            term::session::RestoreTile tile;
            tile.activeTabIndex = 1;

            term::session::Connection ssh;
            ssh.label = "prod-server";
            {
                term::transport::SshDesc d;
                d.host              = "prod.example.com";
                d.port              = 22;
                d.username          = "deploy";
                d.authMethod        = term::transport::SshAuthMethod::PrivateKey;
                d.privateKeyPath    = "/home/user/.ssh/id_rsa";
                d.publicKeyPath     = "/home/user/.ssh/id_rsa.pub";
                d.keepaliveSeconds  = 30;
                d.connectTimeoutSec = 10;
                d.remoteCommand     = "";
                d.compress          = true;
                d.agentForwarding   = true;
                d.password          = "s3cr3t";    // must NOT survive round-trip
                d.passphrase        = "mypass";    // must NOT survive round-trip
                ssh.transport = d;
            }
            tile.sessions.push_back({ssh});

            term::session::Connection ser;
            ser.label = "serial-device";
            {
                term::transport::SerialDesc d;
                d.device      = "/dev/ttyUSB0";
                d.baudRate    = 115200;
                d.dataBits    = 8;
                d.stopBits    = term::transport::SerialStopBits::One;
                d.parity      = term::transport::SerialParity::None;
                d.flowControl = term::transport::SerialFlowControl::Hardware;
                d.dialScript  = "ATZ\r";
                ser.transport = d;
            }
            tile.sessions.push_back({ser});
            w.tiles.push_back(std::move(tile));
        }

        state.windows.push_back(std::move(w));
    }

    // Window 2: loopback session
    {
        term::session::RestoreWindow w;
        w.x = 200; w.y = 100; w.width = 800; w.height = 600;
        term::session::RestoreTile tile;
        tile.activeTabIndex = 0;
        term::session::Connection lb;
        lb.label     = "loopback";
        lb.transport = term::transport::LoopbackDesc{};
        tile.sessions.push_back({lb});
        w.tiles.push_back(std::move(tile));
        state.windows.push_back(std::move(w));
    }

    return state;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("given a valid state when saved and loaded then all fields round-trip", "[restore]")
{
    const std::string path = TmpPath();
    Cleanup(path);

    term::db::JsonSessionRestoreRepository repo(path);
    const auto original = MakeSampleState();
    repo.Save(original);

    REQUIRE(repo.HasSnapshot());
    const auto loaded = repo.Load();

    REQUIRE(loaded.windows.size() == 2);

    // Window 1 geometry
    CHECK(loaded.windows[0].x == 100);
    CHECK(loaded.windows[0].y == 50);
    CHECK(loaded.windows[0].width == 1280);
    CHECK(loaded.windows[0].height == 720);

    // Window 1, tile 1: PTY
    REQUIRE(loaded.windows[0].tiles.size() == 2);
    CHECK(loaded.windows[0].tiles[0].activeTabIndex == 0);
    REQUIRE(loaded.windows[0].tiles[0].sessions.size() == 1);
    {
        const auto& c = loaded.windows[0].tiles[0].sessions[0].conn;
        CHECK(c.label == "local-shell");
        CHECK(c.wrapMode == true);
        CHECK(c.columnWidth == 132);
        CHECK(c.rows == 50);
        CHECK(c.sessionInit.workingDir == "/tmp");
        CHECK(c.sessionInit.loginShell == true);
        const auto* pty = std::get_if<term::transport::PtyDesc>(&c.transport);
        REQUIRE(pty != nullptr);
        CHECK(pty->shell == "/bin/bash");
    }

    // Window 1, tile 2: SSH + Serial; active tab 1
    CHECK(loaded.windows[0].tiles[1].activeTabIndex == 1);
    REQUIRE(loaded.windows[0].tiles[1].sessions.size() == 2);
    {
        const auto& c = loaded.windows[0].tiles[1].sessions[0].conn;
        CHECK(c.label == "prod-server");
        const auto* ssh = std::get_if<term::transport::SshDesc>(&c.transport);
        REQUIRE(ssh != nullptr);
        CHECK(ssh->host == "prod.example.com");
        CHECK(ssh->port == 22);
        CHECK(ssh->username == "deploy");
        CHECK(ssh->authMethod == term::transport::SshAuthMethod::PrivateKey);
        CHECK(ssh->privateKeyPath == "/home/user/.ssh/id_rsa");
        CHECK(ssh->publicKeyPath == "/home/user/.ssh/id_rsa.pub");
        CHECK(ssh->keepaliveSeconds == 30);
        CHECK(ssh->connectTimeoutSec == 10);
        CHECK(ssh->compress == true);
        CHECK(ssh->agentForwarding == true);
    }
    {
        const auto& c = loaded.windows[0].tiles[1].sessions[1].conn;
        CHECK(c.label == "serial-device");
        const auto* ser = std::get_if<term::transport::SerialDesc>(&c.transport);
        REQUIRE(ser != nullptr);
        CHECK(ser->device == "/dev/ttyUSB0");
        CHECK(ser->baudRate == 115200u);
        CHECK(ser->dataBits == 8);
        CHECK(ser->stopBits == term::transport::SerialStopBits::One);
        CHECK(ser->parity == term::transport::SerialParity::None);
        CHECK(ser->flowControl == term::transport::SerialFlowControl::Hardware);
        CHECK(ser->dialScript == "ATZ\r");
    }

    // Window 2: loopback
    CHECK(loaded.windows[1].x == 200);
    CHECK(loaded.windows[1].y == 100);
    REQUIRE(loaded.windows[1].tiles.size() == 1);
    {
        const auto& c = loaded.windows[1].tiles[0].sessions[0].conn;
        CHECK(c.label == "loopback");
        CHECK(std::holds_alternative<term::transport::LoopbackDesc>(c.transport));
    }

    Cleanup(path);
}

TEST_CASE("given SSH session with password when saved then password and passphrase are absent after load", "[restore]")
{
    const std::string path = TmpPath();
    Cleanup(path);

    term::db::JsonSessionRestoreRepository repo(path);
    auto state = MakeSampleState();
    repo.Save(state);

    const auto loaded = repo.Load();
    const auto* ssh = std::get_if<term::transport::SshDesc>(
        &loaded.windows[0].tiles[1].sessions[0].conn.transport);
    REQUIRE(ssh != nullptr);
    CHECK(ssh->password.empty());
    CHECK(ssh->passphrase.empty());

    Cleanup(path);
}

TEST_CASE("given no file when HasSnapshot called then returns false", "[restore]")
{
    const std::string path = TmpPath();
    Cleanup(path);

    term::db::JsonSessionRestoreRepository repo(path);
    CHECK_FALSE(repo.HasSnapshot());
}

TEST_CASE("given empty state when saved then HasSnapshot returns false", "[restore]")
{
    const std::string path = TmpPath();
    Cleanup(path);

    term::db::JsonSessionRestoreRepository repo(path);
    repo.Save(term::session::RestoreState{});

    CHECK_FALSE(repo.HasSnapshot());
    Cleanup(path);
}

TEST_CASE("given valid state when saved then HasSnapshot returns true", "[restore]")
{
    const std::string path = TmpPath();
    Cleanup(path);

    term::db::JsonSessionRestoreRepository repo(path);
    repo.Save(MakeSampleState());

    CHECK(repo.HasSnapshot());
    Cleanup(path);
}

TEST_CASE("given existing snapshot when Delete called then HasSnapshot returns false", "[restore]")
{
    const std::string path = TmpPath();
    Cleanup(path);

    term::db::JsonSessionRestoreRepository repo(path);
    repo.Save(MakeSampleState());
    REQUIRE(repo.HasSnapshot());

    repo.Delete();
    CHECK_FALSE(repo.HasSnapshot());
}
