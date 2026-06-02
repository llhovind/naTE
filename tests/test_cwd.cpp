#include <catch2/catch_test_macros.hpp>
#include "session/Connection.h"
#include "session/Session.h"
#include "ui/DocLayout.h"

using namespace term::session;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

Connection loopbackConn()
{
    return Connection{"cwd-test", LoopbackDesc{}};
}

// Feed the OSC 7 sequence: ESC ] 7 ; <uri> BEL
std::string osc7(const std::string& uri)
{
    return "\033]7;" + uri + "\x07";
}

} // namespace

// ---------------------------------------------------------------------------
// OSC 7 → Session::lastCwd_
// ---------------------------------------------------------------------------

TEST_CASE("given OSC 7 with file URI when processed then GetCurrentWorkingDir returns path") {
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    session.OnData(osc7("file://myhostname/home/user/projects"));
    REQUIRE(session.GetCurrentWorkingDir() == "/home/user/projects");
}

TEST_CASE("given OSC 7 with root-only URI when processed then GetCurrentWorkingDir returns slash") {
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    session.OnData(osc7("file://hostname/"));
    REQUIRE(session.GetCurrentWorkingDir() == "/");
}

TEST_CASE("given OSC 7 with bare path when processed then GetCurrentWorkingDir returns path") {
    // Some shells emit OSC 7 without the file:// scheme.
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    session.OnData(osc7("/tmp/work"));
    REQUIRE(session.GetCurrentWorkingDir() == "/tmp/work");
}

TEST_CASE("given multiple OSC 7 emissions when processed then last value wins") {
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    session.OnData(osc7("file://host/home/user"));
    session.OnData(osc7("file://host/home/user/projects/naTE"));
    REQUIRE(session.GetCurrentWorkingDir() == "/home/user/projects/naTE");
}

TEST_CASE("given OSC 7 with ST terminator when processed then path captured") {
    // OSC sequences may be terminated by ST (ESC \) instead of BEL.
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    const std::string seq = "\033]7;file://host/var/log\033\\";
    session.OnData(seq);
    REQUIRE(session.GetCurrentWorkingDir() == "/var/log");
}

// ---------------------------------------------------------------------------
// ITransportTarget::OnCwdChanged (pwd subchannel fallback)
// ---------------------------------------------------------------------------

TEST_CASE("given OnCwdChanged called directly when GetCurrentWorkingDir called then returns path") {
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    // Simulate what SshTransport fires before OnDisconnect.
    session.OnCwdChanged("/opt/app");
    REQUIRE(session.GetCurrentWorkingDir() == "/opt/app");
}

TEST_CASE("given OSC 7 set then OnCwdChanged called when GetCurrentWorkingDir called then OnCwdChanged wins") {
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    session.OnData(osc7("file://host/home/user"));
    session.OnCwdChanged("/opt/final");
    REQUIRE(session.GetCurrentWorkingDir() == "/opt/final");
}

// ---------------------------------------------------------------------------
// Fallback to transport when lastCwd_ is empty
// ---------------------------------------------------------------------------

TEST_CASE("given no CWD notification when GetCurrentWorkingDir called then falls back to transport") {
    // LoopbackTransport::GetCurrentWorkingDir returns "" — the same as an SSH
    // transport that has never captured a path.  Confirms the fallback is exercised.
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    REQUIRE(session.GetCurrentWorkingDir() == "");
}

// ---------------------------------------------------------------------------
// Parser robustness: double-ESC must not render [6n as text
// ---------------------------------------------------------------------------

TEST_CASE("given OSC 7 with malformed ST followed by CSI when processed then OSC dispatched and no text rendered") {
    // Busybox printf drops a lone trailing '\', producing ESC without the
    // following '\' of the ST terminator.  The next ESC (e.g. from ash's
    // \e[6n cursor-position query) must still trigger OSC dispatch and be
    // processed as a fresh escape sequence — not swallowed, leaving [6n as text.
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    // OSC 7 with bare ESC terminator (no '\'), followed immediately by CSI 6n.
    const std::string seq = "\033]7;file://host/opt/work\033\033[6n";
    session.OnData(seq);
    REQUIRE(session.GetCurrentWorkingDir() == "/opt/work");
    DocLayout& layout = session.GetDocLayout();
    const auto line   = layout.GetRenderedLine(layout.GetLineCount() - 1);
    REQUIRE(line.text.find(U'[') == std::u32string::npos);
    REQUIRE(line.text.find(U'6') == std::u32string::npos);
    REQUIRE(line.text.find(U'n') == std::u32string::npos);
}

TEST_CASE("given double-ESC before CSI 6n when processed then no text rendered") {
    // Shells that emit ESC ESC[6n (e.g. mistakenly doubled reset prefix) must not
    // leave [6n as visible text in the document.  The second ESC should restart
    // the escape sequence rather than dropping the parser back to Normal state.
    Session session(loopbackConn(), 1000, 80, 24, {}, {});
    session.OnData("\x1b\x1b[6n");   // ESC ESC CSI 6 n
    DocLayout& layout = session.GetDocLayout();
    const int  last   = layout.GetLineCount() - 1;
    const auto line   = layout.GetRenderedLine(last);
    // The rendered line must not contain '[', '6', or 'n' from the sequence.
    REQUIRE(line.text.find(U'[') == std::u32string::npos);
    REQUIRE(line.text.find(U'6') == std::u32string::npos);
    REQUIRE(line.text.find(U'n') == std::u32string::npos);
}
