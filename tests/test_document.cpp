#include <catch2/catch_test_macros.hpp>
#include "document/Document.h"

// ---------------------------------------------------------------------------
// DocLine::WriteAt
// ---------------------------------------------------------------------------

TEST_CASE("given empty line when WriteAt col 0 then character appended") {
    DocLine line;
    line.currentStyle = Style{};
    line.WriteAt(0, U'A');

    REQUIRE(line.text == U"A");
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].start  == 0);
    REQUIRE(line.styles[0].length == 1);
}

TEST_CASE("given line with content when WriteAt at end then character appended") {
    DocLine line;
    line.WriteAt(0, U'A');
    line.WriteAt(1, U'B');

    REQUIRE(line.text == U"AB");
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].length == 2);
}

TEST_CASE("given line with content when WriteAt overwrites middle then text updated") {
    DocLine line;
    line.WriteAt(0, U'A');
    line.WriteAt(1, U'B');
    line.WriteAt(2, U'C');

    line.WriteAt(1, U'X');

    REQUIRE(line.text == U"AXC");
}

TEST_CASE("given line when WriteAt overwrites with same style then single run preserved") {
    DocLine line;
    line.currentStyle = Style{};
    line.WriteAt(0, U'A');
    line.WriteAt(1, U'B');
    line.WriteAt(2, U'C');

    line.WriteAt(1, U'X');

    // Same style — no run splitting needed, single run covers the whole line
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].length == 3);
}

TEST_CASE("given line when WriteAt overwrites with different style then runs split") {
    DocLine line;
    line.currentStyle = Style{1, -1, false};  // fg=1 (red)
    line.WriteAt(0, U'A');
    line.WriteAt(1, U'B');
    line.WriteAt(2, U'C');
    // Styles: [{0, 3, fg=1}]

    line.currentStyle = Style{2, -1, false};  // fg=2 (green)
    line.WriteAt(1, U'X');
    // Expected runs: [{0,1,fg=1}, {1,1,fg=2}, {2,1,fg=1}]

    REQUIRE(line.text == U"AXC");
    REQUIRE(line.styles.size() == 3);
    REQUIRE(line.styles[0].start  == 0); REQUIRE(line.styles[0].length == 1);
    REQUIRE(line.styles[1].start  == 1); REQUIRE(line.styles[1].length == 1);
    REQUIRE(line.styles[2].start  == 2); REQUIRE(line.styles[2].length == 1);
    REQUIRE(line.styles[0].style == (Style{1, -1, false}));
    REQUIRE(line.styles[1].style == (Style{2, -1, false}));
    REQUIRE(line.styles[2].style == (Style{1, -1, false}));
}

TEST_CASE("given line when WriteAt overwrites first char then only after-run remains") {
    DocLine line;
    line.currentStyle = Style{1, -1, false};
    line.WriteAt(0, U'A');
    line.WriteAt(1, U'B');
    // [{0, 2, fg=1}]

    line.currentStyle = Style{2, -1, false};
    line.WriteAt(0, U'X');
    // [{0,1,fg=2}, {1,1,fg=1}]

    REQUIRE(line.text == U"XB");
    REQUIRE(line.styles.size() == 2);
    REQUIRE(line.styles[0].start == 0); REQUIRE(line.styles[0].length == 1);
    REQUIRE(line.styles[1].start == 1); REQUIRE(line.styles[1].length == 1);
}

TEST_CASE("given line when WriteAt overwrites last char then only before-run remains") {
    DocLine line;
    line.currentStyle = Style{1, -1, false};
    line.WriteAt(0, U'A');
    line.WriteAt(1, U'B');
    // [{0, 2, fg=1}]

    line.currentStyle = Style{2, -1, false};
    line.WriteAt(1, U'X');
    // [{0,1,fg=1}, {1,1,fg=2}]

    REQUIRE(line.text == U"AX");
    REQUIRE(line.styles.size() == 2);
    REQUIRE(line.styles[0].start == 0); REQUIRE(line.styles[0].length == 1);
    REQUIRE(line.styles[1].start == 1); REQUIRE(line.styles[1].length == 1);
}

TEST_CASE("given empty line when WriteAt past end then padded with spaces") {
    DocLine line;
    line.currentStyle = Style{};
    line.WriteAt(3, U'Z');

    REQUIRE(line.text.size() == 4);
    REQUIRE(line.text[0] == U' ');
    REQUIRE(line.text[1] == U' ');
    REQUIRE(line.text[2] == U' ');
    REQUIRE(line.text[3] == U'Z');
}

// ---------------------------------------------------------------------------
// MainScreenDocument::CarriageReturn
// ---------------------------------------------------------------------------

TEST_CASE("given line with content when CarriageReturn then line not cleared") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'H');
    doc.AppendInsertChar(U'i');

    doc.CarriageReturn();

    REQUIRE(doc.GetLines().back().text == U"Hi");
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given CarriageReturn when characters appended then they overwrite from col 0") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');
    doc.AppendInsertChar(U'B');
    doc.AppendInsertChar(U'C');

    doc.CarriageReturn();
    doc.AppendInsertChar(U'X');
    doc.AppendInsertChar(U'Y');

    // "XYC" — first two chars overwritten, third survives
    REQUIRE(doc.GetLines().back().text == U"XYC");
    REQUIRE(doc.GetCursor().col == 2);
}

TEST_CASE("given progress bar pattern when full line redrawn then line stays same length") {
    // Simulates: write N chars, \r, write N chars of equal length
    MainScreenDocument doc;
    const std::u32string first  = U"Progress: 50%";
    const std::u32string second = U"Progress: 99%";

    for (char32_t ch : first)  doc.AppendInsertChar(ch);
    doc.CarriageReturn();
    for (char32_t ch : second) doc.AppendInsertChar(ch);

    REQUIRE(doc.GetLines().back().text == second);
    REQUIRE(doc.GetLines().size() == 1);
}

TEST_CASE("given progress bar pattern when shorter line redrawn then tail of old line remains") {
    MainScreenDocument doc;
    for (char32_t ch : std::u32string(U"ABCDE")) doc.AppendInsertChar(ch);
    doc.CarriageReturn();
    for (char32_t ch : std::u32string(U"XY"))   doc.AppendInsertChar(ch);

    // First two chars overwritten; "CDE" tail survives
    REQUIRE(doc.GetLines().back().text == U"XYCDE");
}
