#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <vector>

#include "input/InputRouter.h"
#include "input/KeyEvent.hpp"
#include "session/Connection.h"
#include "session/ISessionObserver.h"
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
