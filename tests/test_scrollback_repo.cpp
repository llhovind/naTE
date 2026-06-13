#include <catch2/catch_test_macros.hpp>
#include "db/JsonScrollbackRepository.h"
#include "db/ScrollbackWriter.h"
#include "db/ScrollbackPurge.h"
#include "db/DocumentSerialisation.h"
#include "document/Document.h"
#include "session/RestoreState.h"
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace term::db;
using namespace term::session;
using namespace term::transport;

namespace {

// Unique temp directory per test, cleaned up on destruction.
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& suffix = "") {
        path = fs::temp_directory_path() / ("nate_test_" + suffix + "_" +
                                             std::to_string(
                                                 std::chrono::steady_clock::now()
                                                     .time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};

DocLine MakeLine(const std::u32string& text, int fg = -1)
{
    DocLine line;
    line.text = text;
    if (fg != -1) {
        Style s;
        s.fg = fg;
        line.styles.push_back({0, text.size(), s});
    }
    return line;
}

void WriteNdjsonFile(const std::string& path,
                     const std::vector<DocLine>& lines,
                     bool saveStyles = true)
{
    std::ofstream f(path);
    for (const auto& l : lines)
        f << SerialiseDocLine(l, saveStyles).dump() << "\n";
}

} // namespace

// ---------------------------------------------------------------------------
// DocumentSerialisation round-trip
// ---------------------------------------------------------------------------

TEST_CASE("given styled DocLine when serialised and deserialised then text and styles preserved") {
    DocLine orig = MakeLine(U"Hello", 3);

    const auto j    = SerialiseDocLine(orig, true);
    const DocLine rt = DeserialiseDocLine(j);

    REQUIRE(rt.text == U"Hello");
    REQUIRE(rt.styles.size() == 1);
    REQUIRE(rt.styles[0].style.fg == 3);
}

TEST_CASE("given DocLine when serialised without styles then deserialised line has no style runs") {
    DocLine orig = MakeLine(U"plain", 5);

    const auto j    = SerialiseDocLine(orig, false);
    const DocLine rt = DeserialiseDocLine(j);

    REQUIRE(rt.text == U"plain");
    REQUIRE(rt.styles.empty());
}

// ---------------------------------------------------------------------------
// JsonScrollbackRepository — compact file (clean-exit path)
// ---------------------------------------------------------------------------

TEST_CASE("given compact file when Load then lines returned") {
    TempDir tmp("repo_compact");
    const std::string uuid = "test-uuid-1";
    std::vector<DocLine> written = {MakeLine(U"line1"), MakeLine(U"line2")};
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson", written);

    JsonScrollbackRepository repo(tmp.str());
    auto snap = repo.Load(uuid, 1000);

    REQUIRE(snap.lines.size() == 2);
    REQUIRE(snap.lines[0].text == U"line1");
    REQUIRE(snap.lines[1].text == U"line2");
}

TEST_CASE("given compact file with more lines than maxLines when Load then truncated to maxLines") {
    TempDir tmp("repo_maxlines");
    const std::string uuid = "test-uuid-max";
    std::vector<DocLine> written;
    for (int i = 0; i < 10; ++i)
        written.push_back(MakeLine(U"line" + std::u32string(1, U'0' + i)));
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson", written);

    JsonScrollbackRepository repo(tmp.str());
    auto snap = repo.Load(uuid, 5);

    REQUIRE(snap.lines.size() == 5);
    REQUIRE(snap.lines[0].text == U"line5");
}

// ---------------------------------------------------------------------------
// JsonScrollbackRepository — segment files (crash-recovery path)
// ---------------------------------------------------------------------------

TEST_CASE("given segment files when Load then segment files preferred over compact") {
    TempDir tmp("repo_segments");
    const std::string uuid = "test-uuid-seg";
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson",   {MakeLine(U"stale")});
    WriteNdjsonFile(tmp.str() + "/" + uuid + "-0.ndjson", {MakeLine(U"seg0")});
    WriteNdjsonFile(tmp.str() + "/" + uuid + "-1.ndjson", {MakeLine(U"seg1")});

    JsonScrollbackRepository repo(tmp.str());
    auto snap = repo.Load(uuid, 1000);

    // Segments take precedence; "stale" from compact should not appear.
    REQUIRE(snap.lines.size() == 2);
    REQUIRE(snap.lines[0].text == U"seg0");
    REQUIRE(snap.lines[1].text == U"seg1");
}

// ---------------------------------------------------------------------------
// JsonScrollbackRepository — Exists / Delete / ListAll
// ---------------------------------------------------------------------------

TEST_CASE("given uuid with no files when Exists then returns false") {
    TempDir tmp("repo_exists");
    JsonScrollbackRepository repo(tmp.str());
    REQUIRE_FALSE(repo.Exists("no-such-uuid"));
}

TEST_CASE("given compact file when Exists then returns true") {
    TempDir tmp("repo_exists2");
    const std::string uuid = "exists-uuid";
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson", {});
    JsonScrollbackRepository repo(tmp.str());
    REQUIRE(repo.Exists(uuid));
}

TEST_CASE("given segment file when Exists then returns true") {
    TempDir tmp("repo_exists3");
    const std::string uuid = "exists-seg";
    WriteNdjsonFile(tmp.str() + "/" + uuid + "-0.ndjson", {});
    JsonScrollbackRepository repo(tmp.str());
    REQUIRE(repo.Exists(uuid));
}

TEST_CASE("given existing files when Delete then all three patterns removed") {
    TempDir tmp("repo_delete");
    const std::string uuid = "del-uuid";
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson",   {});
    WriteNdjsonFile(tmp.str() + "/" + uuid + "-0.ndjson", {});
    WriteNdjsonFile(tmp.str() + "/" + uuid + "-1.ndjson", {});

    JsonScrollbackRepository repo(tmp.str());
    repo.Delete(uuid);

    REQUIRE_FALSE(repo.Exists(uuid));
}

TEST_CASE("given multiple uuid files when ListAll then each uuid returned once") {
    TempDir tmp("repo_listall");
    const std::string a = "aaaa-1111";
    const std::string b = "bbbb-2222";
    WriteNdjsonFile(tmp.str() + "/" + a + ".ndjson",   {});
    WriteNdjsonFile(tmp.str() + "/" + b + "-0.ndjson", {});
    WriteNdjsonFile(tmp.str() + "/" + b + "-1.ndjson", {});

    JsonScrollbackRepository repo(tmp.str());
    auto list = repo.ListAll();

    std::sort(list.begin(), list.end());
    REQUIRE(list.size() == 2);
    REQUIRE(list[0] == a);
    REQUIRE(list[1] == b);
}

// ---------------------------------------------------------------------------
// ScrollbackWriter — basic write and compact
// ---------------------------------------------------------------------------

TEST_CASE("given writer with snapshot when Compact then compact file readable via repo") {
    TempDir tmp("writer_compact");
    const std::string uuid = "writer-uuid";
    MainScreenDocument doc(100);

    ScrollbackSnapshot snap;
    snap.lines = {MakeLine(U"alpha"), MakeLine(U"beta")};

    ScrollbackWriter writer(doc, tmp.str(), uuid, 5000, true);
    writer.Start(&snap);
    writer.Compact();

    JsonScrollbackRepository repo(tmp.str());
    auto loaded = repo.Load(uuid, 1000);

    REQUIRE(loaded.lines.size() == 2);
    REQUIRE(loaded.lines[0].text == U"alpha");
    REQUIRE(loaded.lines[1].text == U"beta");
}

TEST_CASE("given writer with initial snapshot when Start then segment 0 pre-populated") {
    TempDir tmp("writer_initial");
    const std::string uuid = "init-uuid";
    MainScreenDocument doc(100);

    ScrollbackSnapshot snap;
    snap.lines = {MakeLine(U"restored-1"), MakeLine(U"restored-2")};

    ScrollbackWriter writer(doc, tmp.str(), uuid, 5000, false);
    writer.Start(&snap);
    writer.Compact();

    JsonScrollbackRepository repo(tmp.str());
    auto loaded = repo.Load(uuid, 1000);
    REQUIRE(loaded.lines.size() == 2);
    REQUIRE(loaded.lines[0].text == U"restored-1");
}

TEST_CASE("given writer when Truncate then all files removed") {
    TempDir tmp("writer_truncate");
    const std::string uuid = "trunc-uuid";
    MainScreenDocument doc(100);

    ScrollbackSnapshot snap;
    snap.lines = {MakeLine(U"data")};

    ScrollbackWriter writer(doc, tmp.str(), uuid, 5000, true);
    writer.Start(&snap);
    writer.Compact();
    writer.Truncate();

    JsonScrollbackRepository repo(tmp.str());
    REQUIRE_FALSE(repo.Exists(uuid));
}

TEST_CASE("given writer after Truncate when Start with new data then fresh segment written") {
    TempDir tmp("writer_truncate_restart");
    const std::string uuid = "trunc-restart";
    MainScreenDocument doc(100);

    ScrollbackWriter writer(doc, tmp.str(), uuid, 5000, true);
    {
        ScrollbackSnapshot old;
        old.lines = {MakeLine(U"old-data")};
        writer.Start(&old);
    }
    writer.Truncate();
    {
        ScrollbackSnapshot fresh;
        fresh.lines = {MakeLine(U"new-data")};
        writer.Start(&fresh);
    }
    writer.Compact();

    JsonScrollbackRepository repo(tmp.str());
    auto loaded = repo.Load(uuid, 1000);
    REQUIRE(loaded.lines.size() == 1);
    REQUIRE(loaded.lines[0].text == U"new-data");
}

// ---------------------------------------------------------------------------
// ScrollbackWriter — rotation
// ---------------------------------------------------------------------------

TEST_CASE("given writer with limit 3 when 4 lines written then rotation creates second segment") {
    TempDir tmp("writer_rotate");
    const std::string uuid = "rotate-uuid";
    MainScreenDocument doc(100);

    ScrollbackSnapshot snap;
    snap.lines = {MakeLine(U"l1"), MakeLine(U"l2"), MakeLine(U"l3"), MakeLine(U"l4")};

    // scrollbackSaveLines=3 triggers rotation after 3 lines written.
    ScrollbackWriter writer(doc, tmp.str(), uuid, 3, true);
    writer.Start(&snap);

    // Verify both segment files exist.
    const std::string seg0 = tmp.str() + "/" + uuid + "-0.ndjson";
    const std::string seg1 = tmp.str() + "/" + uuid + "-1.ndjson";
    REQUIRE(fs::exists(seg0));
    REQUIRE(fs::exists(seg1));
}

// ---------------------------------------------------------------------------
// ScrollbackWriter — concurrent Truncate + OnDocumentChanged (Fix 4)
// ---------------------------------------------------------------------------

TEST_CASE("given writer when Truncate called concurrently with writes then no crash or deadlock") {
    TempDir tmp("writer_concurrent");
    const std::string uuid = "concurrent-uuid";
    MainScreenDocument doc(100);

    ScrollbackWriter writer(doc, tmp.str(), uuid, 10'000, false);
    writer.Start();

    // Simulate the parser thread repeatedly firing OnDocumentChanged while
    // the UI thread calls Truncate. We verify this doesn't deadlock.
    std::atomic<bool> stop{false};
    std::thread writerThread([&] {
        while (!stop.load()) {
            // Directly call OnDocumentChanged — in the real app this fires from
            // the parser thread. lineIndex=0 is skipped so we use 1, but since
            // the document is empty the bounds check in the writer exits cleanly.
            writer.OnDocumentChanged(DocChangeType::InsertLine, 1);
        }
    });

    for (int i = 0; i < 20; ++i)
        writer.Truncate();

    stop.store(true);
    writerThread.join();
    // If we get here without deadlock the test passes.
    REQUIRE(true);
}

// ---------------------------------------------------------------------------
// PurgeOrphanedScrollback
// ---------------------------------------------------------------------------

TEST_CASE("given all uuids referenced when PurgeOrphanedScrollback then nothing deleted") {
    TempDir tmp("purge_all_ref");
    const std::string uuid = "ref-uuid";
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson", {});

    JsonScrollbackRepository repo(tmp.str());

    RestoreSession rs;
    rs.scrollbackUuid = uuid;
    RestoreTile rt;
    rt.sessions.push_back(std::move(rs));
    RestoreWindow rw;
    rw.tiles.push_back(std::move(rt));
    RestoreState state;
    state.windows.push_back(std::move(rw));

    PurgeOrphanedScrollback(repo, {state});

    REQUIRE(repo.Exists(uuid));
}

TEST_CASE("given orphaned uuid when PurgeOrphanedScrollback then file deleted") {
    TempDir tmp("purge_orphan");
    const std::string orphan   = "orphan-uuid";
    const std::string kept     = "kept-uuid";
    WriteNdjsonFile(tmp.str() + "/" + orphan + ".ndjson", {});
    WriteNdjsonFile(tmp.str() + "/" + kept   + ".ndjson", {});

    JsonScrollbackRepository repo(tmp.str());

    RestoreSession rs;
    rs.scrollbackUuid = kept;
    RestoreTile rt;
    rt.sessions.push_back(std::move(rs));
    RestoreWindow rw;
    rw.tiles.push_back(std::move(rt));
    RestoreState state;
    state.windows.push_back(std::move(rw));

    PurgeOrphanedScrollback(repo, {state});

    REQUIRE_FALSE(repo.Exists(orphan));
    REQUIRE(repo.Exists(kept));
}

TEST_CASE("given empty state list when PurgeOrphanedScrollback then all files deleted") {
    TempDir tmp("purge_empty_states");
    const std::string uuid = "nobody-refs-this";
    WriteNdjsonFile(tmp.str() + "/" + uuid + ".ndjson", {});

    JsonScrollbackRepository repo(tmp.str());
    PurgeOrphanedScrollback(repo, {});

    REQUIRE_FALSE(repo.Exists(uuid));
}

TEST_CASE("given empty directory when PurgeOrphanedScrollback then no error") {
    TempDir tmp("purge_empty_dir");
    JsonScrollbackRepository repo(tmp.str());
    REQUIRE_NOTHROW(PurgeOrphanedScrollback(repo, {}));
}
