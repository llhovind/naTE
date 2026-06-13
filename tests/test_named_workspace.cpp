#include <catch2/catch_test_macros.hpp>
#include "db/JsonNamedWorkspaceRepository.h"
#include "session/RestoreState.h"
#include "session/Connection.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string TmpDir()
{
    return (fs::temp_directory_path() / "nate_named_workspace_test").string();
}

static void CleanupDir(const std::string& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}

static term::session::RestoreState MakeState(const std::string& label = "session-a")
{
    term::session::RestoreState state;
    term::session::RestoreWindow w;
    w.x = 10; w.y = 20; w.width = 800; w.height = 600;
    term::session::RestoreTile tile;
    tile.activeTabIndex = 0;
    term::session::Connection c;
    c.label     = label;
    c.transport = term::transport::LoopbackDesc{};
    tile.sessions.push_back({c});
    w.tiles.push_back(std::move(tile));
    state.windows.push_back(std::move(w));
    return state;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("given empty dir when List called then returns empty vector", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    CHECK(repo.List().empty());

    CleanupDir(dir);
}

TEST_CASE("given state when saved with name then Exists returns true", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("my-workspace", MakeState());

    CHECK(repo.Exists("my-workspace"));
    CHECK_FALSE(repo.Exists("other"));

    CleanupDir(dir);
}

TEST_CASE("given saved workspace when loaded then all fields round-trip", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("round-trip", MakeState("loopback-session"));

    const auto loaded = repo.Load("round-trip");
    REQUIRE(loaded.windows.size() == 1);
    REQUIRE(loaded.windows[0].tiles.size() == 1);
    REQUIRE(loaded.windows[0].tiles[0].sessions.size() == 1);
    CHECK(loaded.windows[0].tiles[0].sessions[0].conn.label == "loopback-session");
    CHECK(loaded.windows[0].x == 10);
    CHECK(loaded.windows[0].y == 20);
    CHECK(loaded.windows[0].width  == 800);
    CHECK(loaded.windows[0].height == 600);

    CleanupDir(dir);
}

TEST_CASE("given two saves with different names when List called then both names returned sorted",
          "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("zebra", MakeState());
    repo.Save("alpha", MakeState());

    const auto names = repo.List();
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "alpha");
    CHECK(names[1] == "zebra");

    CleanupDir(dir);
}

TEST_CASE("given existing workspace when Delete called then Exists returns false", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("to-delete", MakeState());
    REQUIRE(repo.Exists("to-delete"));

    repo.Delete("to-delete");
    CHECK_FALSE(repo.Exists("to-delete"));

    CleanupDir(dir);
}

TEST_CASE("given deleted workspace when List called then name is absent", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("keep",   MakeState());
    repo.Save("remove", MakeState());
    repo.Delete("remove");

    const auto names = repo.List();
    REQUIRE(names.size() == 1);
    CHECK(names[0] == "keep");

    CleanupDir(dir);
}

TEST_CASE("given name with spaces and hyphens when saved then loads correctly", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("my dev setup", MakeState("ssh-session"));

    REQUIRE(repo.Exists("my dev setup"));
    const auto loaded = repo.Load("my dev setup");
    REQUIRE(loaded.windows[0].tiles[0].sessions[0].conn.label == "ssh-session");

    CleanupDir(dir);
}

TEST_CASE("given existing workspace when overwritten then Load returns new state", "[named_workspace]")
{
    const std::string dir = TmpDir();
    CleanupDir(dir);

    term::db::JsonNamedWorkspaceRepository repo(dir);
    repo.Save("workspace", MakeState("original"));
    repo.Save("workspace", MakeState("updated"));

    const auto loaded = repo.Load("workspace");
    REQUIRE(loaded.windows[0].tiles[0].sessions[0].conn.label == "updated");

    CleanupDir(dir);
}
