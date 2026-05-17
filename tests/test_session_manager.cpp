#include <catch2/catch_test_macros.hpp>
#include <atomic>
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
    enum class Type { Disconnected, Error, Destroyed };
    Type      type;
    SessionId id;
};

struct MockObserver : ISessionObserver {
    std::vector<RecordedEvent> events;

    void OnSessionDisconnected(SessionId id) override {
        events.push_back({RecordedEvent::Type::Disconnected, id});
    }
    void OnSessionError(SessionId id,
                        const term::transport::TransportError& /*error*/) override {
        events.push_back({RecordedEvent::Type::Error, id});
    }
    void OnSessionDestroyed(SessionId id) override {
        events.push_back({RecordedEvent::Type::Destroyed, id});
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

TEST_CASE("given observer when session closed then OnSessionDestroyed fires") {
    term::input::InputRouter router;
    SessionManager sm;
    MockObserver obs;

    const SessionId id = sm.CreateSession(loopbackConn("My Session"), 1000, 80, 24);
    sm.SetSessionObserver(id, &obs);

    sm.CloseSession(id);

    const auto destroyed = obs.ofType(RecordedEvent::Type::Destroyed);
    REQUIRE(destroyed.size() == 1);
    REQUIRE(destroyed[0].id == id);
}

TEST_CASE("given two sessions when second closed then only second fires destroyed") {
    term::input::InputRouter router;
    SessionManager sm;
    MockObserver obs;

    const SessionId id1 = sm.CreateSession(loopbackConn("A"), 1000, 80, 24);
    const SessionId id2 = sm.CreateSession(loopbackConn("B"), 1000, 80, 24);
    sm.SetSessionObserver(id1, &obs);
    sm.SetSessionObserver(id2, &obs);

    sm.CloseSession(id2);

    const auto destroyed = obs.ofType(RecordedEvent::Type::Destroyed);
    REQUIRE(destroyed.size() == 1);
    REQUIRE(destroyed[0].id == id2);
}

TEST_CASE("given session when GetDocLayout called then layout has at least one line") {
    SessionManager sm;

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    DocLayout& layout = sm.GetDocLayout(id);
    REQUIRE(layout.GetLineCount() >= 1);
}

TEST_CASE("given no session when GetDocLayout called with unknown id then throws") {
    SessionManager sm;

    REQUIRE_THROWS_AS(sm.GetDocLayout(999), std::out_of_range);
}

TEST_CASE("given session when SetSessionObserver changes observer then disconnect routes to new observer") {
    SessionManager sm;
    MockObserver obs1;
    MockObserver obs2;

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);
    sm.SetSessionObserver(id, &obs1);

    // Reassign to obs2 — destroyed notification should go to obs2.
    sm.SetSessionObserver(id, &obs2);
    sm.CloseSession(id);

    REQUIRE(obs1.ofType(RecordedEvent::Type::Destroyed).empty());
    REQUIRE(obs2.ofType(RecordedEvent::Type::Destroyed).size() == 1);
}

TEST_CASE("given session when ReassignRouter called then session moves between routers") {
    term::input::InputRouter router1;
    term::input::InputRouter router2;
    SessionManager sm;

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);
    sm.RegisterRouter(id, router1);

    // Activate in router1 — send a character; LoopbackTransport echoes it back.
    sm.ActivateSession(id, router1);
    term::input::KeyEvent evt;
    evt.key  = term::input::Key::Character;
    evt.code = 'x';
    evt.text = "x";
    router1.Send(evt);  // should not crash

    // Move to router2 — subsequent sends go via router2.
    sm.ReassignRouter(id, router2);
    sm.ActivateSession(id, router2);
    router2.Send(evt);  // should not crash

    // Session is still accessible and healthy.
    REQUIRE(sm.GetDocLayout(id).GetLineCount() >= 1);

    sm.CloseSession(id);
}

TEST_CASE("given session when MakeTitleGetter called then returns session title") {
    SessionManager sm;

    const SessionId id = sm.CreateSession(loopbackConn("TitleTest"), 1000, 80, 24);
    auto getter = sm.MakeTitleGetter(id);
    REQUIRE_NOTHROW(getter());

    sm.CloseSession(id);
}

TEST_CASE("given session when GetLabel called then returns connection label") {
    SessionManager sm;

    const SessionId id = sm.CreateSession(loopbackConn("MyLabel"), 1000, 80, 24);
    REQUIRE(sm.GetLabel(id) == "MyLabel");

    sm.CloseSession(id);
}

TEST_CASE("given session when closed twice then second call is a no-op") {
    term::input::InputRouter router;
    SessionManager sm;
    MockObserver obs;

    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);
    sm.SetSessionObserver(id, &obs);
    sm.CloseSession(id);
    obs.events.clear();

    // Second close must not crash or fire additional events.
    sm.CloseSession(id);

    REQUIRE(obs.events.empty());
}

// ---------------------------------------------------------------------------
// Parser → Session → Document integration: mid-line delete sequences
// ---------------------------------------------------------------------------

TEST_CASE("given bash readline backspace sequence with CSI K when processed then last char cleared") {
    // Simulates the PTY data stream for backward-delete-char at position 5 in
    // "abcdefghijk":
    //   initial text written: "abcdefghijk"
    //   cursor navigated left 6:  ESC[D × 6
    //   bash response:         \b  + "fghijk" + ESC[K + ESC[6D
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

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
    Session session(conn, 1000, 80, 24, {}, {});

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

TEST_CASE("given Home key when encoded then produces CSI H") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Home;
    REQUIRE(enc.Encode(evt) == "\x1b[H");
}

TEST_CASE("given End key when encoded then produces CSI F") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::End;
    REQUIRE(enc.Encode(evt) == "\x1b[F");
}

TEST_CASE("given Delete key when encoded then produces CSI 3-tilde") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Delete;
    REQUIRE(enc.Encode(evt) == "\x1b[3~");
}

TEST_CASE("given Insert key when encoded then produces CSI 2-tilde") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::Insert;
    REQUIRE(enc.Encode(evt) == "\x1b[2~");
}

TEST_CASE("given PageUp key when encoded then produces CSI 5-tilde") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key = term::input::Key::PageUp;
    REQUIRE(enc.Encode(evt) == "\x1b[5~");
}

TEST_CASE("given PageDown key when encoded then produces CSI 6-tilde") {
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

TEST_CASE("given F5 key when encoded then produces CSI 15-tilde") {
    term::session::InputEncoder enc;
    term::input::KeyEvent evt;
    evt.key  = term::input::Key::FunctionKey;
    evt.code = 5;
    REQUIRE(enc.Encode(evt) == "\x1b[15~");
}

TEST_CASE("given F12 key when encoded then produces CSI 24-tilde") {
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

TEST_CASE("given CSI H when processed then cursor column resets to 0") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    // "hello" leaves cursor at col 5; ESC[H moves it to (1,1) → col 0; 'X' overwrites col 0
    session.OnData("hello\033[HX");

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text[0] == U'X');
}

TEST_CASE("given CSI 3-tilde when processed then character at cursor is deleted") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2 (on 'c')
    session.OnData("\033[3~");            // tilde Delete → DeleteChar(1)

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abde");
}

TEST_CASE("given CSI P when processed then character at cursor is deleted") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[P");             // DCH → DeleteChar(1)

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abde");
}

TEST_CASE("given CSI 2P when processed then two characters at cursor are deleted") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[2P");            // delete 2 chars → removes 'c' and 'd'

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abe");
}

TEST_CASE("given CSI 2J when processed then all lines cleared") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("line one\n");
    session.OnData("line two\n");
    session.OnData("line three");
    session.OnData("\033[2J");

    REQUIRE(session.GetDocLayout().GetLineCount() == 1);
    REQUIRE(session.GetDocLayout().GetRenderedLine(0).text.empty());
    REQUIRE(session.GetDocLayout().GetCursorDocPos().line == 0);
    REQUIRE(session.GetDocLayout().GetCursorDocPos().col  == 0);
}

TEST_CASE("given CSI J when processed then content from cursor to end is erased") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[J");             // erase from cursor to end of display

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"ab");
}

TEST_CASE("given CSI 1-tilde when processed then cursor moves to line start") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    // "abcde" puts cursor at col 5; ESC[1~ (Home tilde) moves it to col 0;
    // 'X' overwrites position 0 → line starts with 'X'
    session.OnData("abcde\033[1~X");

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text[0] == U'X');
    REQUIRE(line.text == U"Xbcde");
}

TEST_CASE("given CSI 4-tilde when processed then cursor moves to line end") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    // Move back to col 2, then End tilde moves to col 5, 'X' appends
    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[4~X");           // End tilde → col 5 → append 'X'

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abcdeX");
}

TEST_CASE("given CSI F when processed then cursor moves to line end") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2
    session.OnData("\033[FX");            // ESC[F → end → append 'X'

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abcdeX");
}

TEST_CASE("given CSI ?25h and CSI ?25l when processed then no crash") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    // Cursor visibility sequences are no-ops at Session level but must parse cleanly.
    REQUIRE_NOTHROW(session.OnData("\033[?25h"));
    REQUIRE_NOTHROW(session.OnData("\033[?25l"));
}

TEST_CASE("given SS3 and tilde F-key sequences when processed then no crash") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

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
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("abcde");
    session.OnData("\033[D\033[D\033[D"); // cursor at col 2

    term::input::KeyEvent evt;
    evt.key = term::input::Key::Delete;
    session.OnInput(evt); // encoder → ESC[3~ → loopback → parser → DeleteChar(1)

    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.text == U"abde");
}

// ---------------------------------------------------------------------------
// Parser: SGR sequences → Style in Document
// ---------------------------------------------------------------------------

TEST_CASE("given basic fg SGR code when processed then style stores palette index 0 to 7") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("\033[31mX"); // red fg → palette index 1
    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE_FALSE(line.attrs.empty());
    REQUIRE(line.attrs.back().fg == 1);
}

TEST_CASE("given bright fg SGR code when processed then style stores palette index 8 to 15") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("\033[91mX"); // bright red fg → palette index 9
    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE_FALSE(line.attrs.empty());
    REQUIRE(line.attrs.back().fg == 9);
}

TEST_CASE("given 256-color fg SGR sequence when processed then style stores palette index") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("\033[38;5;196mX"); // xterm colour 196 (bright red cube)
    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE_FALSE(line.attrs.empty());
    REQUIRE(line.attrs.back().fg == 196);
}

TEST_CASE("given 256-color bg SGR sequence when processed then style stores palette index") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    session.OnData("\033[48;5;21mX"); // xterm colour 21 (blue cube) as bg
    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE_FALSE(line.attrs.empty());
    REQUIRE(line.attrs.back().bg == 21);
}

TEST_CASE("given SGR reset sequence when processed then style returns to defaults") {
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 80, 24, {}, {});

    // Set red fg, then reset; 'X' must have default style
    session.OnData("\033[31mA\033[0mX");
    const RenderedLine line = session.GetDocLayout().GetRenderedLine(
        session.GetDocLayout().GetLineCount() - 1);
    REQUIRE(line.attrs.size() >= 2);
    REQUIRE(line.attrs.back().fg == -1);
    REQUIRE(line.attrs.back().bg == -1);
}

TEST_CASE("given line longer than viewport when wrap mode off then horizontal scroll slices correctly") {
    // 20-column viewport; wrap mode defaults to false
    Connection conn{"test", LoopbackDesc{}};
    Session session(conn, 1000, 20, 5, {}, {});

    // Feed 40 printable characters: A-Z then A-N
    session.OnData("ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMN");

    DocLayout& layout = session.GetDocLayout();

    // No-wrap: entire input is one visual row
    REQUIRE(layout.GetLineCount() == 1);

    // EnsureCursorVisibleHorizontally auto-scrolled to keep the cursor (col 40)
    // in view — verify leftCol advanced past 0.
    REQUIRE(layout.GetLeftCol() > 0);

    // Manually reset to leftCol=0 to verify viewport slicing from the start.
    layout.SetLeftCol(0);
    const RenderedLine row0 = layout.GetRenderedLine(0);
    REQUIRE(row0.text == U"ABCDEFGHIJKLMNOPQRST");

    // Scroll right by 10 — chars at positions 10..29 should be visible.
    layout.SetLeftCol(10);
    const RenderedLine row1 = layout.GetRenderedLine(0);
    REQUIRE(row1.text == U"KLMNOPQRSTUVWXYZABCD");
}

// ---------------------------------------------------------------------------
// MakeTitleGetter profile-title overload
// ---------------------------------------------------------------------------

TEST_CASE("given useProfileTitle=true and non-empty profileTitle when MakeTitleGetter called then getter returns profileTitle")
{
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    auto getter = sm.MakeTitleGetter(id, "My Box", true);
    CHECK(getter() == "My Box");

    sm.CloseSession(id);
}

TEST_CASE("given useProfileTitle=false when MakeTitleGetter called then getter returns session document title")
{
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    auto getter = sm.MakeTitleGetter(id, "Ignored", false);
    // Loopback session has no title set — empty string expected
    CHECK(getter() == "");

    sm.CloseSession(id);
}

TEST_CASE("given useProfileTitle=true but empty profileTitle when MakeTitleGetter called then getter falls back to document title")
{
    SessionManager sm;
    const SessionId id = sm.CreateSession(loopbackConn(), 1000, 80, 24);

    auto getter = sm.MakeTitleGetter(id, "", true);
    // Empty profileTitle — should fall through to document title (also empty for loopback)
    CHECK(getter() == "");

    sm.CloseSession(id);
}
