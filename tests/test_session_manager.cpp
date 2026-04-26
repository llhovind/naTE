#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <vector>

#include "input/InputRouter.h"
#include "input/KeyEvent.hpp"
#include "session/Connection.h"
#include "session/ISessionObserver.h"
#include "session/InputEncoder.h"
#include "session/Session.h"
#include "session/SessionManager.h"

using namespace term::session;

namespace {

struct RecordedEvent {
    enum class Type { Created, TitleChanged, Refresh, Disconnected, Destroyed };
    Type      type;
    SessionId id;
    std::string data;
};

struct MockObserver : ISessionObserver {
    std::vector<RecordedEvent> events;

    void OnSessionCreated(SessionId id, const std::string& label) override {
        events.push_back({RecordedEvent::Type::Created, id, label});
    }
    void OnSessionTitleChanged(SessionId id, const std::string& title) override {
        events.push_back({RecordedEvent::Type::TitleChanged, id, title});
    }
    void OnSessionRefresh(SessionId id) override {
        events.push_back({RecordedEvent::Type::Refresh, id, {}});
    }
    void OnSessionDisconnected(SessionId id) override {
        events.push_back({RecordedEvent::Type::Disconnected, id, {}});
    }
    void OnSessionDestroyed(SessionId id) override {
        events.push_back({RecordedEvent::Type::Destroyed, id, {}});
    }

    std::vector<RecordedEvent> ofType(RecordedEvent::Type t) const {
        std::vector<RecordedEvent> out;
        for (const auto& e : events)
            if (e.type == t) out.push_back(e);
        return out;
    }
};

Connection loopbackConn(const std::string& label = "Test") {
    return Connection{label, LoopbackDesc{}};
}

} // namespace

TEST_CASE("given observer when session created then OnSessionCreated fires with label") {
    term::input::InputRouter router;
    SessionManager sm(router);
    MockObserver obs;
    sm.SetObserver(&obs);

    const SessionId id = sm.CreateSession(loopbackConn("My Session"), 1000, 80, 24);

    const auto created = obs.ofType(RecordedEvent::Type::Created);
    REQUIRE(created.size() == 1);
    REQUIRE(created[0].id   == id);
    REQUIRE(created[0].data == "My Session");
}

TEST_CASE("given live session when closed then OnSessionDestroyed fires") {
    term::input::InputRouter router;
    SessionManager sm(router);
    MockObserver obs;
    sm.SetObserver(&obs);

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);
    obs.events.clear();

    sm.CloseSession(id);

    const auto destroyed = obs.ofType(RecordedEvent::Type::Destroyed);
    REQUIRE(destroyed.size() == 1);
    REQUIRE(destroyed[0].id == id);
}

TEST_CASE("given two sessions when ActivateSession called then active id tracks correctly") {
    term::input::InputRouter router;
    SessionManager sm(router);
    MockObserver obs;
    sm.SetObserver(&obs);

    const SessionId id1 = sm.CreateSession(loopbackConn("A"), 1000, 80, 24);
    const SessionId id2 = sm.CreateSession(loopbackConn("B"), 1000, 80, 24);

    sm.ActivateSession(id1);
    REQUIRE(sm.GetActiveSessionId() == id1);

    sm.ActivateSession(id2);
    REQUIRE(sm.GetActiveSessionId() == id2);
}

TEST_CASE("given session when GetDocLayout called then layout has at least one line") {
    term::input::InputRouter router;
    SessionManager sm(router);
    MockObserver obs;
    sm.SetObserver(&obs);

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    DocLayout& layout = sm.GetDocLayout(id);
    REQUIRE(layout.GetLineCount() >= 1);
}

TEST_CASE("given no session when GetDocLayout called with unknown id then throws") {
    term::input::InputRouter router;
    SessionManager sm(router);

    REQUIRE_THROWS_AS(sm.GetDocLayout(999), std::out_of_range);
}

TEST_CASE("given loopback session when character input sent then OnSessionRefresh fires") {
    // LoopbackTransport echoes writes back synchronously: no threading involved.
    // The chain is: router.Send → session.OnInput → encoder → transport.Write
    //   → OnData → parser → document mutation → DocumentObserver → OnSessionRefresh.
    term::input::InputRouter router;
    SessionManager sm(router);
    MockObserver obs;
    sm.SetObserver(&obs);

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);
    sm.ActivateSession(id);
    obs.events.clear();

    term::input::KeyEvent evt;
    evt.key  = term::input::Key::Character;
    evt.code = 'a';
    evt.text = "a";
    router.Send(evt);

    REQUIRE_FALSE(obs.ofType(RecordedEvent::Type::Refresh).empty());
}

// ---------------------------------------------------------------------------
// Parser → Session → Document integration: mid-line delete sequences
// ---------------------------------------------------------------------------

TEST_CASE("given bash readline backspace sequence with ESC[K when processed then last char cleared") {
    // Simulates the PTY data stream for backward-delete-char at position 5 in
    // "abcdefghijk":
    //   initial text written: "abcdefghijk"
    //   cursor navigated left 6:  ESC[D × 6
    //   bash response:         \b  + "fghijk" + ESC[K + ESC[6D
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    // Write the initial text directly into the document via OnData
    session.OnData("abcdefghijk");
    // Navigate cursor to position 5 (between 'e' and 'f')
    session.OnData("\033[D\033[D\033[D\033[D\033[D\033[D");
    // Bash readline backward-delete-char response:
    //   \b          → cursor 5 → 4 (on 'e')
    //   fghijk      → overwrite positions 4–9, cursor → 10  (text: "abcdfghijkk")
    //   ESC[K       → EraseInLine(0): erase from col 10 to end  (text: "abcdfghijk")
    //   ESC[6D      → cursor back to 4
    session.OnData("\bfghijk\033[K\033[6D");

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abcdfghijk");
}

TEST_CASE("given bash readline backspace sequence with space-overwrite when processed then last char cleared") {
    // Same scenario but bash uses a trailing space (dumb-terminal style) instead of ESC[K.
    //   bash response:  \b + "fghijk" + " " + \b × 7
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcdefghijk");
    session.OnData("\033[D\033[D\033[D\033[D\033[D\033[D");
    session.OnData("\bfghijk \b\b\b\b\b\b\b");

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    // The trailing space replaces the old 'k'; line ends with a space.
    // Visual display will show "abcdfghijk" (space is invisible vs background).
    REQUIRE(line.text == U"abcdfghijk ");
}

// ---------------------------------------------------------------------------
// InputEncoder: new key encodings
// ---------------------------------------------------------------------------

TEST_CASE("given Home key when encoded then produces ESC[H") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Home;
    REQUIRE(enc.Encode(evt) == "\x1b[H");
}

TEST_CASE("given End key when encoded then produces ESC[F") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::End;
    REQUIRE(enc.Encode(evt) == "\x1b[F");
}

TEST_CASE("given Delete key when encoded then produces ESC[3~") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Delete;
    REQUIRE(enc.Encode(evt) == "\x1b[3~");
}

TEST_CASE("given Insert key when encoded then produces ESC[2~") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Insert;
    REQUIRE(enc.Encode(evt) == "\x1b[2~");
}

TEST_CASE("given PageUp key when encoded then produces ESC[5~") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::PageUp;
    REQUIRE(enc.Encode(evt) == "\x1b[5~");
}

TEST_CASE("given PageDown key when encoded then produces ESC[6~") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::PageDown;
    REQUIRE(enc.Encode(evt) == "\x1b[6~");
}

TEST_CASE("given Escape key when encoded then produces ESC") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Escape;
    REQUIRE(enc.Encode(evt) == "\x1b");
}

TEST_CASE("given F1 key when encoded then produces SS3 P sequence") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key  = term::input::Key::FunctionKey;
    evt.code = 1;
    REQUIRE(enc.Encode(evt) == "\x1bOP");
}

TEST_CASE("given F4 key when encoded then produces SS3 S sequence") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key  = term::input::Key::FunctionKey;
    evt.code = 4;
    REQUIRE(enc.Encode(evt) == "\x1bOS");
}

TEST_CASE("given F5 key when encoded then produces ESC[15~") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key  = term::input::Key::FunctionKey;
    evt.code = 5;
    REQUIRE(enc.Encode(evt) == "\x1b[15~");
}

TEST_CASE("given F12 key when encoded then produces ESC[24~") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key  = term::input::Key::FunctionKey;
    evt.code = 12;
    REQUIRE(enc.Encode(evt) == "\x1b[24~");
}

// ---------------------------------------------------------------------------
// Parser: new CSI sequences → Document state
// Cursor position is verified indirectly through content effects, since
// AppendInsertChar always writes to the active (last) doc line at cursor_.col.
// ---------------------------------------------------------------------------

TEST_CASE("given ESC[H when processed then cursor column resets to 0") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    // "hello" leaves cursor at col 5; ESC[H moves it to (1,1) → col 0; 'X' overwrites col 0
    session.OnData("hello\033[HX");

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text[0] == U'X');
}

TEST_CASE("given ESC[3~ when processed then character at cursor is deleted") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2 (on 'c')
    session.OnData("\033[3~");            // tilde Delete → DeleteChar(1)

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abde");
}

TEST_CASE("given ESC[P when processed then character at cursor is deleted") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[P");             // DCH → DeleteChar(1)

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abde");
}

TEST_CASE("given ESC[2P when processed then two characters at cursor are deleted") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[2P");            // delete 2 chars → removes 'c' and 'd'

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abe");
}

TEST_CASE("given ESC[2J when processed then all lines cleared") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("line one\n");
    session.OnData("line two\n");
    session.OnData("line three");
    session.OnData("\033[2J");

    REQUIRE(session.GetDocLayout().GetLineCount() == 1);
    REQUIRE(session.GetDocLayout().GetRenderedLine(0).text.empty());
    REQUIRE(session.GetDocLayout().GetCursorDocPos().line == 0);
    REQUIRE(session.GetDocLayout().GetCursorDocPos().col  == 0);
}

TEST_CASE("given ESC[J when processed then content from cursor to end is erased") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[J");             // erase from cursor to end of display

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"ab");
}

TEST_CASE("given ESC[1~ when processed then cursor moves to line start") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    // "abcde" puts cursor at col 5; ESC[1~ (Home tilde) moves it to col 0;
    // 'X' overwrites position 0 → line starts with 'X'
    session.OnData("abcde\033[1~X");

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text[0] == U'X');
    REQUIRE(line.text == U"Xbcde");
}

TEST_CASE("given ESC[4~ when processed then cursor moves to line end") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    // Move back to col 2, then End tilde moves to col 5, 'X' appends
    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[4~X");           // End tilde → col 5 → append 'X'

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abcdeX");
}

TEST_CASE("given ESC[F when processed then cursor moves to line end") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[FX");            // ESC[F → end → append 'X'

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abcdeX");
}

TEST_CASE("given ESC[?25h and ESC[?25l when processed then no crash") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    // Cursor visibility sequences are no-ops at Session level but must parse cleanly.
    REQUIRE_NOTHROW(session.OnData("\033[?25h"));
    REQUIRE_NOTHROW(session.OnData("\033[?25l"));
}

TEST_CASE("given SS3 and tilde F-key sequences when processed then no crash") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    // F-key sequences from PTY output are no-ops at Session level but must
    // parse without crashing.
    REQUIRE_NOTHROW(session.OnData("\033OP"));   // F1 SS3
    REQUIRE_NOTHROW(session.OnData("\033OQ"));   // F2 SS3
    REQUIRE_NOTHROW(session.OnData("\033[15~")); // F5 tilde
    REQUIRE_NOTHROW(session.OnData("\033[24~")); // F12 tilde
}

TEST_CASE("given Delete key loopback when processed then char at cursor removed") {
    // Full round-trip: KeyEvent → InputEncoder → LoopbackTransport → Parser → Document.
    // Uses Session directly so we can pre-feed document state via OnData.
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2

    term::input::KeyEvent evt;
    evt.key = term::input::Key::Delete;
    session.OnInput(evt); // encoder → ESC[3~ → loopback → parser → DeleteChar(1)

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abde");
}

TEST_CASE("given session when closed twice then second call is a no-op") {
    term::input::InputRouter router;
    SessionManager sm(router);
    MockObserver obs;
    sm.SetObserver(&obs);

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);
    sm.CloseSession(id);
    obs.events.clear();

    // Second close must not crash or fire additional events.
    sm.CloseSession(id);

    REQUIRE(obs.events.empty());
}
