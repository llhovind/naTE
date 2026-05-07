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

// ---------------------------------------------------------------------------
// Mid-line delete via bash readline sequence:
//   \b  +  rewrite-shifted-chars  +  ESC[K  +  cursor-reposition
// ---------------------------------------------------------------------------

TEST_CASE("given readline mid-line delete sequence when EraseInLine sent then last char cleared") {
    // Simulates bash readline backward-delete-char at position 5 in "abcdefghijk":
    //   \b              → MoveCursorLeft(1)
    //   "fghijk"        → 6 AppendInsertChar calls (shift overwrite)
    //   ESC[K (mode 0)  → EraseInLine(0)
    //   cursor back     → MoveCursorLeft(6)
    MainScreenDocument doc;
    for (char32_t ch : std::u32string(U"abcdefghijk")) doc.AppendInsertChar(ch);
    // cursor at 11; navigate to position 5 (just after 'e') then \b moves to 4
    doc.MoveCursorLeft(6);   // cursor = 5
    doc.MoveCursorLeft(1);   // cursor = 4  (\b)
    for (char32_t ch : std::u32string(U"fghijk")) doc.AppendInsertChar(ch);
    doc.EraseInLine(0);       // ESC[K — clear from cursor to end
    doc.MoveCursorLeft(6);   // cursor back to 4

    REQUIRE(doc.GetLines().back().text == U"abcdfghijk");
    REQUIRE(doc.GetCursor().col == 4);
}

TEST_CASE("given readline mid-line delete sequence when space-overwrite used then last char cleared") {
    // Same scenario but bash uses a trailing space instead of ESC[K
    MainScreenDocument doc;
    for (char32_t ch : std::u32string(U"abcdefghijk")) doc.AppendInsertChar(ch);
    doc.MoveCursorLeft(6);   // cursor = 5
    doc.MoveCursorLeft(1);   // cursor = 4  (\b)
    for (char32_t ch : std::u32string(U"fghijk")) doc.AppendInsertChar(ch);
    // cursor is now at 10; 'k' still at 10 — overwrite with space
    doc.AppendInsertChar(U' ');
    doc.MoveCursorLeft(7);   // cursor back to 4

    REQUIRE(doc.GetLines().back().text == U"abcdfghijk ");
    REQUIRE(doc.GetCursor().col == 4);
}

// ---------------------------------------------------------------------------
// AltScreenDocument — fixed-size grid
// ---------------------------------------------------------------------------

TEST_CASE("given 3x5 grid when MoveCursorDown at last row then cursor stays clamped") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorToPosition(3, 1);   // row 3 (last), col 1
    doc.MoveCursorDown(10);
    REQUIRE(doc.GetCursor().line == 2);
}

TEST_CASE("given 3x5 grid when MoveCursorUp at row 0 then cursor stays at row 0") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorUp(5);
    REQUIRE(doc.GetCursor().line == 0);
}

TEST_CASE("given 3x5 grid when MoveCursorRight at last col then cursor stays clamped") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorRight(100);
    REQUIRE(doc.GetCursor().col == 4);
}

TEST_CASE("given 3x5 grid when AppendInsertChar at col boundary then cursor wraps to next row") {
    AltScreenDocument doc(3, 5);
    // Fill row 0 (5 chars) — cursor should wrap to row 1 col 0
    for (int i = 0; i < 5; ++i)
        doc.AppendInsertChar(U'A');
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 0);
}

TEST_CASE("given 3x5 grid when AppendInsertChar at last row last col then cursor stays at bottom-right") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorToPosition(3, 5);   // last row, last col (1-indexed)
    doc.AppendInsertChar(U'Z');
    // After writing, col would overflow but clamp at rows_-1 row
    REQUIRE(doc.GetCursor().line == 2);
}

TEST_CASE("given 3x5 grid when NewLine at last row then grid scrolls and cursor stays at bottom") {
    AltScreenDocument doc(3, 5);
    // Write a marker on row 0
    doc.AppendInsertChar(U'X');
    doc.MoveCursorToPosition(3, 1);   // move to last row
    doc.NewLine();
    REQUIRE(doc.GetCursor().line == 2);
    // Row 0 marker should have scrolled — row 0 is now the old row 1 (blank)
    REQUIRE(doc.GetLines()[0].text.empty());
    REQUIRE(doc.GetLines().size() == 3);
}

TEST_CASE("given 3x5 grid when EraseInDisplay mode 2 then all lines blank and cursor at origin") {
    AltScreenDocument doc(3, 5);
    doc.AppendInsertChar(U'A');
    doc.MoveCursorToPosition(2, 3);
    doc.AppendInsertChar(U'B');
    doc.EraseInDisplay(2);
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 0);
    for (const auto& line : doc.GetLines())
        REQUIRE(line.text.empty());
}

TEST_CASE("given 3x5 grid when MoveCursorToPosition then cursor is clamped to grid") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorToPosition(2, 3);   // 1-indexed → row 1, col 2 (0-indexed)
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 2);
}

TEST_CASE("given 3x5 grid when MoveCursorToPosition out of bounds then cursor clamped to grid") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorToPosition(99, 99);
    REQUIRE(doc.GetCursor().line == 2);
    REQUIRE(doc.GetCursor().col  == 4);
}

TEST_CASE("given 3x5 grid when Resize to 4x6 then grid has 4 rows and cursor clamped") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorToPosition(3, 5);   // last position in 3x5
    doc.Resize(4, 6);
    REQUIRE(doc.GetLines().size() == 4);
    REQUIRE(doc.GetCursor().line <= 3);
    REQUIRE(doc.GetCursor().col  <= 5);
}

TEST_CASE("given 3x5 grid when Resize to 2x4 then cursor clamped to new bounds") {
    AltScreenDocument doc(3, 5);
    doc.MoveCursorToPosition(3, 5);   // row=2,col=4 (0-indexed)
    doc.Resize(2, 4);
    REQUIRE(doc.GetLines().size() == 2);
    REQUIRE(doc.GetCursor().line <= 1);
    REQUIRE(doc.GetCursor().col  <= 3);
}

TEST_CASE("given 3x5 grid when EraseInLine mode 0 then content from cursor to end cleared") {
    AltScreenDocument doc(3, 5);
    for (char32_t ch : std::u32string(U"ABCDE")) doc.AppendInsertChar(ch);
    // Writing 5 chars fills row 0 and wraps cursor to row 1. Go back to row 0 col 2.
    doc.MoveCursorToPosition(1, 3);   // 1-indexed: row 1, col 3 → 0-indexed row 0, col 2
    doc.EraseInLine(0);
    REQUIRE(doc.GetLines()[0].text == U"AB");
}

TEST_CASE("given 3x5 grid when NewLine not at bottom then cursor moves down without scrolling") {
    AltScreenDocument doc(3, 5);
    doc.NewLine();
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetLines().size() == 3);
}

// ---------------------------------------------------------------------------
// MainScreenDocument virtual-row cursor translation (SetPtyCols)
// ---------------------------------------------------------------------------

TEST_CASE("given ptyCols=80 when MoveCursorUp(1) from col 90 then cursor moves to col 10") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    // Place cursor at col 90 by moving right from 0
    doc.MoveCursorRight(90);
    doc.MoveCursorUp(1);
    REQUIRE(doc.GetCursor().col == 10);
}

TEST_CASE("given ptyCols=80 when MoveCursorUp(1) from col 10 then cursor clamps to col 0") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(10);
    doc.MoveCursorUp(1);
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=80 when MoveCursorDown(1) from col 10 then cursor moves to col 90") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(10);
    doc.MoveCursorDown(1);
    REQUIRE(doc.GetCursor().col == 90);
}

// ---------------------------------------------------------------------------
// NewLine() with wrapped lines (readline [End] key fix)
// ---------------------------------------------------------------------------

TEST_CASE("given ptyCols=80 and 90-char line when NewLine from subRow 0 then cursor advances to subRow 1") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    for (int i = 0; i < 90; ++i) doc.AppendInsertChar(U'A');
    doc.MoveCursorLeft(90);
    const size_t linesBefore = doc.GetLines().size();
    doc.NewLine();
    REQUIRE(doc.GetLines().size() == linesBefore); // no new DocLine
    REQUIRE(doc.GetCursor().col == 80);            // start of subRow 1
    REQUIRE(doc.GetCursor().line == linesBefore - 1);
}

TEST_CASE("given ptyCols=80 and 90-char line when NewLine from subRow 1 (last) then new DocLine created") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    for (int i = 0; i < 90; ++i) doc.AppendInsertChar(U'A');
    // cursor_.col == 90, which is on subRow 1 — the last one
    const size_t linesBefore = doc.GetLines().size();
    doc.NewLine();
    REQUIRE(doc.GetLines().size() == linesBefore + 1);
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=80 and empty line when NewLine then new DocLine created") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    const size_t linesBefore = doc.GetLines().size();
    doc.NewLine();
    REQUIRE(doc.GetLines().size() == linesBefore + 1);
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=80 when MoveCursorToPosition(2,5) then only col is used") {
    // row is an absolute terminal row — not a virtual command row — so it is
    // ignored.  Only the col parameter (1-indexed) is applied.
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorToPosition(2, 5);
    REQUIRE(doc.GetCursor().col == 4);
}

TEST_CASE("given ptyCols=80 when MoveCursorToPosition(1,1) then cursor at col 0") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorToPosition(1, 1);
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=80 when MoveCursorToColumn(5) from col 85 then cursor at col 84") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(85);
    doc.MoveCursorToColumn(5);
    // virtual row 1 preserved: 1*80 + (5-1) = 84
    REQUIRE(doc.GetCursor().col == 84);
}

TEST_CASE("given ptyCols=80 when MoveCursorToRow(2) then cursor is unchanged") {
    // CSI d sends an absolute terminal row — ignored like the row in CSI H.
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(10);
    doc.MoveCursorToRow(2);
    REQUIRE(doc.GetCursor().col == 10);
}

TEST_CASE("given ptyCols=80 when CarriageReturn from col 85 then cursor at col 80") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(85);
    doc.CarriageReturn();
    // start of virtual row 1: 1*80 = 80
    REQUIRE(doc.GetCursor().col == 80);
}

TEST_CASE("given ptyCols=80 when CarriageReturn from col 10 then cursor at col 0") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(10);
    doc.CarriageReturn();
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=2048 (default) when MoveCursorUp(1) from col 90 then cursor clamps to 0") {
    MainScreenDocument doc;
    // Default cols_ = 2048, so 1 up = subtract 2048, clamp to 0
    doc.MoveCursorRight(90);
    doc.MoveCursorUp(1);
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=2048 when MoveCursorToPosition(1,91) then cursor at col 90") {
    MainScreenDocument doc;
    // row is always ignored; col 91 (1-indexed) → 90
    doc.MoveCursorToPosition(1, 91);
    REQUIRE(doc.GetCursor().col == 90);
}

TEST_CASE("given ptyCols=10 and 20-char line when readline multi-row clear sequence then no phantom DocLine") {
    // Simulates readline replacing a wrapped command via history cycling:
    //   CursorUp  → EraseInLine(0)  → NewLine  → EraseInLine(0)
    // After the first EraseInLine the second sub-row no longer exists in the
    // text, so NewLine creates a phantom DocLine. The second EraseInLine must
    // detect and remove it.
    MainScreenDocument doc;
    doc.SetPtyCols(10);
    for (int i = 0; i < 20; ++i) doc.AppendInsertChar(U'A'); // 2 sub-rows
    const size_t linesBefore = doc.GetLines().size();
    const int    lineBefore  = doc.GetCursor().line;

    doc.MoveCursorUp(1);     // move into sub-row 0
    doc.EraseInLine(0);      // erase sub-row 0 → text now empty
    doc.NewLine();           // readline thinks sub-row 1 exists; creates phantom DocLine
    doc.EraseInLine(0);      // must remove phantom and restore cursor

    REQUIRE(doc.GetLines().size() == linesBefore);          // no net new DocLine
    REQUIRE(doc.GetCursor().line == lineBefore);            // cursor on original line
    // CursorUp landed at col 10 (sub-row 1 start); EraseInLine(0) removed
    // chars 10-19, leaving "AAAAAAAAAA". Cursor restored to end of that text.
    REQUIRE(doc.GetCursor().col == doc.GetLines()[doc.GetCursor().line].text.size());
}
