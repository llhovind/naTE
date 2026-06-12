#include <catch2/catch_test_macros.hpp>
#include "document/Document.h"
#include "parser/Parser.h"
#include "parser/IScreenTarget.h"

// ---------------------------------------------------------------------------
// DocLine::WriteAt
// ---------------------------------------------------------------------------

TEST_CASE("given empty line when WriteAt col 0 then character appended") {
    DocLine line;
    line.WriteAt(0, U'A', Style{});

    REQUIRE(line.text == U"A");
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].start  == 0);
    REQUIRE(line.styles[0].length == 1);
}

TEST_CASE("given line with content when WriteAt at end then character appended") {
    DocLine line;
    line.WriteAt(0, U'A', Style{});
    line.WriteAt(1, U'B', Style{});

    REQUIRE(line.text == U"AB");
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].length == 2);
}

TEST_CASE("given line with content when WriteAt overwrites middle then text updated") {
    DocLine line;
    line.WriteAt(0, U'A', Style{});
    line.WriteAt(1, U'B', Style{});
    line.WriteAt(2, U'C', Style{});

    line.WriteAt(1, U'X', Style{});

    REQUIRE(line.text == U"AXC");
}

TEST_CASE("given line when WriteAt overwrites with same style then single run preserved") {
    DocLine line;
    line.WriteAt(0, U'A', Style{});
    line.WriteAt(1, U'B', Style{});
    line.WriteAt(2, U'C', Style{});

    line.WriteAt(1, U'X', Style{});

    // Same style — no run splitting needed, single run covers the whole line
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].length == 3);
}

TEST_CASE("given line when WriteAt overwrites with different style then runs split") {
    DocLine line;
    const Style red  {1, -1, false};
    const Style green{2, -1, false};
    line.WriteAt(0, U'A', red);
    line.WriteAt(1, U'B', red);
    line.WriteAt(2, U'C', red);
    // Styles: [{0, 3, fg=1}]

    line.WriteAt(1, U'X', green);
    // Expected runs: [{0,1,fg=1}, {1,1,fg=2}, {2,1,fg=1}]

    REQUIRE(line.text == U"AXC");
    REQUIRE(line.styles.size() == 3);
    REQUIRE(line.styles[0].start  == 0); REQUIRE(line.styles[0].length == 1);
    REQUIRE(line.styles[1].start  == 1); REQUIRE(line.styles[1].length == 1);
    REQUIRE(line.styles[2].start  == 2); REQUIRE(line.styles[2].length == 1);
    REQUIRE(line.styles[0].style == red);
    REQUIRE(line.styles[1].style == green);
    REQUIRE(line.styles[2].style == red);
}

TEST_CASE("given line when WriteAt overwrites first char then only after-run remains") {
    DocLine line;
    const Style red  {1, -1, false};
    const Style green{2, -1, false};
    line.WriteAt(0, U'A', red);
    line.WriteAt(1, U'B', red);
    // [{0, 2, fg=1}]

    line.WriteAt(0, U'X', green);
    // [{0,1,fg=2}, {1,1,fg=1}]

    REQUIRE(line.text == U"XB");
    REQUIRE(line.styles.size() == 2);
    REQUIRE(line.styles[0].start == 0); REQUIRE(line.styles[0].length == 1);
    REQUIRE(line.styles[1].start == 1); REQUIRE(line.styles[1].length == 1);
}

TEST_CASE("given line when WriteAt overwrites last char then only before-run remains") {
    DocLine line;
    const Style red  {1, -1, false};
    const Style green{2, -1, false};
    line.WriteAt(0, U'A', red);
    line.WriteAt(1, U'B', red);
    // [{0, 2, fg=1}]

    line.WriteAt(1, U'X', green);
    // [{0,1,fg=1}, {1,1,fg=2}]

    REQUIRE(line.text == U"AX");
    REQUIRE(line.styles.size() == 2);
    REQUIRE(line.styles[0].start == 0); REQUIRE(line.styles[0].length == 1);
    REQUIRE(line.styles[1].start == 1); REQUIRE(line.styles[1].length == 1);
}

TEST_CASE("given empty line when WriteAt past end then padded with spaces") {
    DocLine line;
    line.WriteAt(3, U'Z', Style{});

    REQUIRE(line.text.size() == 4);
    REQUIRE(line.text[0] == U' ');
    REQUIRE(line.text[1] == U' ');
    REQUIRE(line.text[2] == U' ');
    REQUIRE(line.text[3] == U'Z');
}

// ---------------------------------------------------------------------------
// DocLine::TruncateFrom
// ---------------------------------------------------------------------------

TEST_CASE("given styled line when TruncateFrom mid-run then run truncated and later runs dropped") {
    DocLine line;
    const Style red  {1, -1, false};
    const Style green{2, -1, false};
    for (int i = 0; i < 4; ++i) line.WriteAt(i, U'A', red);    // cols 0-3 red
    for (int i = 4; i < 8; ++i) line.WriteAt(i, U'B', green);  // cols 4-7 green

    line.TruncateFrom(2);

    REQUIRE(line.text == U"AA");
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].start  == 0);
    REQUIRE(line.styles[0].length == 2);
    REQUIRE(line.styles[0].style  == red);
}

TEST_CASE("given line when TruncateFrom at run boundary then trailing runs dropped exactly") {
    DocLine line;
    const Style red  {1, -1, false};
    const Style green{2, -1, false};
    line.WriteAt(0, U'A', red);
    line.WriteAt(1, U'B', green);

    line.TruncateFrom(1);

    REQUIRE(line.text == U"A");
    REQUIRE(line.styles.size() == 1);
    REQUIRE(line.styles[0].style == red);
}

TEST_CASE("given line when TruncateFrom at or past end then line unchanged") {
    DocLine line;
    line.WriteAt(0, U'A', Style{});
    line.WriteAt(1, U'B', Style{});

    line.TruncateFrom(2);
    REQUIRE(line.text == U"AB");
    REQUIRE(line.styles.size() == 1);

    line.TruncateFrom(99);
    REQUIRE(line.text == U"AB");
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

TEST_CASE("given 3x5 grid when AppendInsertChar fills row then cursor is at last col with pending wrap") {
    AltScreenDocument doc(3, 5);
    // VT100 deferred-wrap: filling the last column sets pendingWrap_ but leaves
    // the cursor reported at (0, 4) until the next printable character.
    for (int i = 0; i < 5; ++i)
        doc.AppendInsertChar(U'A');
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 4);

    // The next character triggers the wrap; 'B' is placed at (1, 0) and the
    // cursor then advances to (1, 1).
    doc.AppendInsertChar(U'B');
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 1);
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

TEST_CASE("given ptyCols=80 when MoveCursorToPosition(2,5) then canvas-relative row and col are set") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorToPosition(2, 5);
    // Row 2 relative to virtualDocStartLine_=0 → doc line 1 (blank line appended on demand).
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 4);
}

TEST_CASE("given ptyCols=80 when MoveCursorToPosition(1,1) then cursor at canvas origin") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorToPosition(1, 1);
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 0);
}

TEST_CASE("given ptyCols=80 when MoveCursorToColumn(5) from col 85 then cursor at col 84") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(85);
    doc.MoveCursorToColumn(5);
    // virtual row 1 preserved: 1*80 + (5-1) = 84
    REQUIRE(doc.GetCursor().col == 84);
}

TEST_CASE("given ptyCols=80 when MoveCursorToRow(2) then canvas-relative row is set and col unchanged") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(10);
    doc.MoveCursorToRow(2);
    // Row 2 relative to virtualDocStartLine_=0 → doc line 1; col is not touched.
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 10);
}

// ---------------------------------------------------------------------------
// PtyToDoc correctness — exercised via MoveCursorToPosition / MoveCursorToRow
// ---------------------------------------------------------------------------

TEST_CASE("given ptyCols=80 and 90-char DocLine when MoveCursorToPosition(2,5) then cursor lands on subRow 1 of DocLine 0") {
    // Regression: before PtyToDoc, CUP row 2 with a wrapped first DocLine
    // incorrectly jumped to DocLine 1 instead of sub-row 1 of DocLine 0.
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    for (int i = 0; i < 90; ++i) doc.AppendInsertChar(U'A'); // DocLine 0: 90 chars → 2 PTY rows
    doc.NewLine();                                             // DocLine 1: ""
    doc.MoveCursorToPosition(2, 5);
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 84); // subRow 1 * 80 + (5-1)
}

TEST_CASE("given ptyCols=80 and 90-char DocLine when MoveCursorToPosition(3,1) then cursor lands on DocLine 1") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    for (int i = 0; i < 90; ++i) doc.AppendInsertChar(U'A'); // DocLine 0: 2 PTY rows
    doc.NewLine();                                             // DocLine 1: ""
    doc.MoveCursorToPosition(3, 1);
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 0);
}

TEST_CASE("given ptyCols=80 and exactly 160-char DocLine when MoveCursorToPosition(2,1) then cursor lands on subRow 1") {
    // 160 chars / 80 cols = exactly 2 rows; no padding, no off-by-one.
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    for (int i = 0; i < 160; ++i) doc.AppendInsertChar(U'A');
    doc.NewLine();
    doc.MoveCursorToPosition(2, 1);
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 80); // subRow 1, col 0
}

TEST_CASE("given ptyCols=80 and cursor on subRow 1 when MoveCursorToRow(1) then PTY column is preserved in subRow 0") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorRight(85); // col=85 → subRow 1, PTY col 5 (0-indexed)
    doc.MoveCursorToRow(1);
    // PTY col = 85 % 80 + 1 = 6 (1-indexed); PtyToDoc(1,6) on empty doc → {0, 5}
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 5);
}

TEST_CASE("given ptyCols=80 and 90-char DocLine when MoveCursorToPosition(2,5) then MoveCursorDown(1) is sub-row-consistent") {
    // Before fix: CUP(2,5) set cursor.line=1, so MoveCursorDown targeted the wrong DocLine.
    // After fix: CUP(2,5) places cursor at {0,84}; Down adds 80 → {0,164} = subRow 2, col 4.
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    for (int i = 0; i < 90; ++i) doc.AppendInsertChar(U'A');
    doc.NewLine();
    doc.MoveCursorToPosition(2, 5);
    REQUIRE(doc.GetCursor().line == 0);
    doc.MoveCursorDown(1);
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 164);
}

TEST_CASE("given ptyCols=80 and empty doc when MoveCursorToPosition(3,1) then two blank DocLines are emplaced by PtyToDoc") {
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    doc.MoveCursorToPosition(3, 1);
    // PtyToDoc: lines[0] empty→1 row, lines[1] empty→1 row, land on lines[2]
    REQUIRE(doc.GetLines().size() >= 3);
    REQUIRE(doc.GetCursor().line == 2);
    REQUIRE(doc.GetCursor().col  == 0);
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

TEST_CASE("given ptyCols=2048 when MoveCursorToPosition(1,91) then cursor at row 0 col 90") {
    MainScreenDocument doc;
    doc.MoveCursorToPosition(1, 91);
    REQUIRE(doc.GetCursor().line == 0);
    REQUIRE(doc.GetCursor().col  == 90);
}

TEST_CASE("given fresh document when AdvanceCanvas then virtualDocStartLine advances and one blank line exists") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A'); // writes to line 0; doc still has 1 line: ["A"]
    doc.AdvanceCanvas();
    // virtualDocStartLine_ = 1 (1 prior line); 1 new blank line appended → 2 total
    REQUIRE(doc.GetScrollbackOrigin() == 1);
    REQUIRE(doc.GetLines().size() == 2);
    REQUIRE(doc.GetLines().back().text.empty());
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col  == 0);
}

TEST_CASE("given virtualDocStartLine=1 when MoveCursorToPosition(3,1) then blank lines appended on demand") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');
    doc.AdvanceCanvas(); // virtualDocStartLine_ = 1, lines = ["A", ""]
    doc.MoveCursorToPosition(3, 1);
    // canvas row 3 = abs line 1 + 2 = 3; lines 2 and 3 must be appended on demand
    REQUIRE(doc.GetLines().size() == 4);
    REQUIRE(doc.GetCursor().line == 3);
    REQUIRE(doc.GetCursor().col  == 0);
}

TEST_CASE("given cursor at canvas top when EraseInDisplay(0) then new canvas is started") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');   // line 0: "A"
    doc.NewLine();                 // line 1: ""
    doc.AppendInsertChar(U'B');   // line 1: "B"
    // cursor is at line 1, not canvas top (line 0) — move it there
    doc.MoveCursorToPosition(1, 1); // canvas row 1 = abs line 0
    REQUIRE(doc.GetCursor().line == 0);
    doc.EraseInDisplay(0);
    // AdvanceCanvas fired: virtualDocStartLine_ = 2 (prior "A", "B" lines), new blank appended
    REQUIRE(doc.GetScrollbackOrigin() == 2);
    REQUIRE(doc.GetLines().size() == 3);
    REQUIRE(doc.GetCursor().line == 2);
}

TEST_CASE("given cursor mid-canvas when EraseInDisplay(0) then lines below are blanked not popped") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');
    doc.NewLine();
    doc.AppendInsertChar(U'B');
    doc.NewLine();
    doc.AppendInsertChar(U'C');  // lines: ["A", "B", "C"]
    doc.MoveCursorToPosition(2, 1); // canvas row 2 = abs line 1 ("B")
    const size_t linesBefore = doc.GetLines().size();
    doc.EraseInDisplay(0);
    REQUIRE(doc.GetLines().size() == linesBefore);   // no lines popped
    REQUIRE(doc.GetLines()[1].text.empty());          // "B" blanked
    REQUIRE(doc.GetLines()[2].text.empty());          // "C" blanked
    REQUIRE(doc.GetScrollbackOrigin() == 0);       // no canvas advance
}

TEST_CASE("given EraseInDisplay(1) when called then blanks from canvas origin not from doc line 0") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'X');  // line 0: "X"  ← will become scrollback
    doc.AdvanceCanvas();          // virtualDocStartLine_ = 1, lines: ["X", ""]
    doc.AppendInsertChar(U'A');  // line 1 (canvas start): "A"
    doc.NewLine();                 // line 2: ""
    doc.AppendInsertChar(U'B');  // line 2: "B"  ← cursor here (canvas row 2)
    doc.EraseInDisplay(1);       // erase from canvas origin (line 1) to cursor (line 2)
    REQUIRE(doc.GetLines()[0].text == U"X");  // scrollback line 0 untouched
    REQUIRE(doc.GetLines()[1].text.empty());   // canvas line 0 (was "A") blanked
    // cursor line (line 2) is partially blanked up to col
}

TEST_CASE("given NewLine pops front when maxLines hit then virtualDocStartLine is decremented") {
    MainScreenDocument doc(3); // maxLines = 3
    doc.AppendInsertChar(U'A');
    doc.AdvanceCanvas(); // virtualDocStartLine_ = 2, lines: ["", "A", ""]
    // Adding another line will trigger a pop since maxLines=3
    doc.AppendInsertChar(U'B');
    doc.NewLine();        // lines would be ["", "A", "B", ""] → pop front → ["A", "B", ""]
    REQUIRE(doc.GetLines().size() == 3);
    REQUIRE(doc.GetScrollbackOrigin() == 1); // decremented from 2 to 1
}

TEST_CASE("given SaveCursor then AdvanceCanvas then RestoreCursor then cursor in new canvas") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');
    doc.NewLine();
    doc.AppendInsertChar(U'B');  // cursor at line 1 (canvas row 2)
    doc.SaveCursor();
    doc.AdvanceCanvas();          // virtualDocStartLine_ advances past "A","B"
    doc.RestoreCursor();
    // Saved canvas row 2 → restored as canvas row 2 in new canvas
    REQUIRE(doc.GetCursor().line == doc.GetScrollbackOrigin() + 1);
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

TEST_CASE("given short command and bash CRLF then EraseInLine does not remove the new line") {
    // Regression: bash sends \r\n after a command, then grep --color sends
    // ESC[K at column 0 of the new blank line. The phantom-removal heuristic
    // must NOT fire here because the new DocLine was preceded by \r.
    MainScreenDocument doc;
    doc.SetPtyCols(80);
    // Write a short command (< 80 chars) to simulate a prompt + command line.
    for (int i = 0; i < 32; ++i) doc.AppendInsertChar(U'A');
    const size_t linesBefore = doc.GetLines().size();

    doc.CarriageReturn();  // \r
    doc.NewLine();         // \n  → new DocLine (cursorSubRow == lastSubRow, preceded by CR)
    doc.CarriageReturn();  // bash pre-output \r
    doc.EraseInLine(0);    // grep color ESC[K — must NOT remove the new DocLine

    REQUIRE(doc.GetLines().size() == linesBefore + 1);  // new line preserved
    REQUIRE(doc.GetCursor().line == (int)(linesBefore)); // cursor on new line
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given command of exactly ptyCols chars and bash CRLF then EraseInLine does not remove the new line") {
    // Edge case: command fills exactly one terminal row (cols_ chars). After \r,
    // cursor is at col cols_ (cursorSubRow > lastSubRow — looks like a readline
    // phantom geometrically). The preceding \r must prevent phantom removal.
    MainScreenDocument doc;
    doc.SetPtyCols(10);
    for (int i = 0; i < 10; ++i) doc.AppendInsertChar(U'A'); // exactly cols_ chars
    const size_t linesBefore = doc.GetLines().size();

    doc.CarriageReturn();  // \r  — cursor at col 10 (cursorSubRow=1 > lastSubRow=0)
    doc.NewLine();         // \n  → new DocLine; crPriorToNewLine_ prevents phantom flag
    doc.CarriageReturn();  // bash pre-output \r
    doc.EraseInLine(0);    // must NOT remove the new DocLine

    REQUIRE(doc.GetLines().size() == linesBefore + 1);
    REQUIRE(doc.GetCursor().line == (int)(linesBefore));
    REQUIRE(doc.GetCursor().col == 0);
}

TEST_CASE("given ptyCols=10 and 15-char line when new-readline history-cycle sequence then DocLine contains new command") {
    // Newer readline clears a wrapped line via:
    //   CursorUp → Backspace(n) → DeleteChar(n) → WriteChars → NewLine → EraseInLine(0)
    // DeleteChar must not shift sub-row 1 content into sub-row 0.
    // After the full sequence the DocLine must equal the new command with no
    // phantom line created and cursor at the content end.
    //
    // Setup: cols_=10, "AAAAAAAAAAAAAAA" (15 chars)
    //   sub-row 0: cols 0-9  ("AAAAAAAAAA")
    //   sub-row 1: cols 10-14 ("AAAAA")
    // Prompt is 2 chars ("$ "), so command starts at col 2.
    MainScreenDocument doc;
    doc.SetPtyCols(10);
    // Write "$ " prompt then 13 'A's so total = 15 chars, cursor at col 15
    for (char32_t ch : std::u32string(U"$ AAAAAAAAAAAAA")) doc.AppendInsertChar(ch);

    const size_t linesBefore = doc.GetLines().size();
    const int    lineBefore  = doc.GetCursor().line;

    // CursorUp: col 15 → 15-10 = 5 (into sub-row 0, after prompt)
    doc.MoveCursorUp(1);
    // Backspace back to col 2 (end of "$ " prompt): 3 backspaces
    doc.Backspace(); doc.Backspace(); doc.Backspace();
    REQUIRE(doc.GetCursor().col == 2);
    // DeleteChar(8): fills cols 2-9 with spaces (sub-row 0 end), sub-row 1 untouched
    doc.DeleteChar(8);
    // Write new shorter command "BBBBB" (5 chars)
    for (char32_t ch : std::u32string(U"BBBBB")) doc.AppendInsertChar(ch);
    // NewLine: cursor advances to sub-row 1 start (col 10)
    doc.NewLine();
    REQUIRE(doc.GetCursor().col == 10);
    // EraseInLine(0): erases old sub-row 1 content, trims trailing spaces, resets cursor
    doc.EraseInLine(0);

    REQUIRE(doc.GetLines().size() == linesBefore);         // no phantom DocLine
    REQUIRE(doc.GetCursor().line == lineBefore);           // still on original line
    REQUIRE(doc.GetLines()[lineBefore].text == U"$ BBBBB"); // trimmed to actual content
    REQUIRE(doc.GetCursor().col == doc.GetLines()[lineBefore].text.size());
}

TEST_CASE("given ptyCols=10 and busybox wrap-backspace sequence when MoveCursorUp then rewrite lands on original DocLine") {
    // Busybox readline does not rely on terminal auto-wrap: it explicitly emits
    // \r\n when a command wraps, creating a real DocLine for the wrapped content.
    // When the user backspaces all the way across the wrap, busybox sends:
    //   ESC[J (erase to end) + CR + CUU1 + redrawn-command.
    // ESC[J empties DocLine L+1; CUU1 then sees an empty last DocLine at col=0
    // and must pop it so the rewrite lands on DocLine L.
    //
    // Setup: cols_=10, "AAAAAAAAAA" (10 chars) on DocLine L.
    //   busybox \r\n → DocLine L+1; "BB" echoed there.
    //   User presses backspace twice; busybox echoes each as \b SPACE \b
    //   (space-overwrite, not actual deletion) → L+1 has "  ", cursor at col 0.
    //   Busybox then sends ESC[J CR CUU1 to erase and redraw.
    MainScreenDocument doc;
    doc.SetPtyCols(10);
    // sub-row 0 of the command
    for (int i = 0; i < 10; ++i) doc.AppendInsertChar(U'A');
    REQUIRE(doc.GetCursor().col == 10);

    // busybox \r\n — creates real DocLine L+1
    doc.CarriageReturn();
    doc.NewLine();
    REQUIRE((int)doc.GetLines().size() == 2);
    REQUIRE(doc.GetCursor().col == 0);

    // busybox echoes "BB" onto L+1
    doc.AppendInsertChar(U'B');
    doc.AppendInsertChar(U'B');
    REQUIRE(doc.GetCursor().col == 2);

    const int lineL = doc.GetCursor().line - 1; // DocLine L index

    // Backspace 1: \b SPACE \b  (cursor 2→1, write ' ', cursor 2→1)
    doc.MoveCursorLeft(1);
    doc.AppendInsertChar(U' ');
    doc.MoveCursorLeft(1);
    // Backspace 2: \b SPACE \b  (cursor 1→0, write ' ', cursor 1→0)
    doc.MoveCursorLeft(1);
    doc.AppendInsertChar(U' ');
    doc.MoveCursorLeft(1);

    REQUIRE(doc.GetCursor().col == 0);
    // L+1 has two spaces — not yet empty, ESC[J will clear it
    REQUIRE(doc.GetLines()[doc.GetCursor().line].text == std::u32string(U"  "));

    // busybox: ESC[J empties L+1 from col 0, CR stays at col 0, CUU1 pops L+1
    doc.EraseInDisplay(0);
    REQUIRE(doc.GetLines()[doc.GetCursor().line].text.empty()); // L+1 now empty
    doc.CarriageReturn();
    doc.MoveCursorUp(1);  // must pop L+1 and land at col 0 of DocLine L

    // Rewrite shorter command "AAAAAAAA" (8 chars)
    for (int i = 0; i < 8; ++i) doc.AppendInsertChar(U'A');

    REQUIRE((int)doc.GetLines().size() == lineL + 1); // L+1 was popped
    REQUIRE(doc.GetCursor().line == (size_t)lineL);   // cursor on DocLine L
    REQUIRE(doc.GetCursor().col == 8);                 // after 8 written chars
}

TEST_CASE("given ptyCols=10 and 15-char line when DeleteChar at col 2 then sub-row 1 chars are unchanged") {
    // Directly verifies the subRow boundary invariant: DeleteChar on a
    // multi-subRow line must not shift sub-row 1 content into sub-row 0.
    MainScreenDocument doc;
    doc.SetPtyCols(10);
    for (int i = 0; i < 15; ++i) doc.AppendInsertChar(static_cast<char32_t>(U'A' + i)); // "ABCDEFGHIJKLMNO"
    doc.MoveCursorUp(1);   // cursor: 15 → 5
    doc.MoveCursorLeft(3); // cursor: 5 → 2

    doc.DeleteChar(8); // fill cols 2-9 with spaces; sub-row 1 (cols 10-14) = "KLMNO"

    const auto& text = doc.GetLines()[doc.GetCursor().line].text;
    REQUIRE(text.size() == 15);
    // sub-row 1 content at positions 10-14 must be unchanged
    REQUIRE(text.substr(10) == U"KLMNO");
    // deleted range became spaces
    for (size_t i = 2; i < 10; ++i) REQUIRE(text[i] == U' ');
}

TEST_CASE("given ptyCols=10 and 20-char line when DeleteChar(3) at col 2 then shift within sub-row and sub-row 1 unchanged") {
    // Verifies DeleteAtClamped shift-within-subRow semantics (partial delete, not to EOL).
    // "ABCDEFGHIJ" = sub-row 0 (0-9), "KLMNOPQRST" = sub-row 1 (10-19)
    // DeleteChar(3) at col 2 on sub-row 0:
    //   real terminal: E-F-G-H-I-J shift left to [2..7], [8..9] fill with spaces
    //   sub-row 1 unchanged.
    MainScreenDocument doc;
    doc.SetPtyCols(10);
    for (int i = 0; i < 20; ++i) doc.AppendInsertChar(static_cast<char32_t>(U'A' + i));
    doc.MoveCursorUp(1);   // cursor: 20 → 10 (sub-row 1 start)
    doc.MoveCursorLeft(8); // cursor: 10 → 2

    doc.DeleteChar(3); // delete E,F,G at cols 2,3,4

    const auto& text = doc.GetLines()[doc.GetCursor().line].text;
    REQUIRE(text.size() == 20);
    REQUIRE(text[0] == U'A'); REQUIRE(text[1] == U'B');
    // cols 2-6: F..J shifted left (C,D,E deleted; originally at cols 5-9)
    REQUIRE(text.substr(2, 5) == U"FGHIJ");
    // cols 7-9: spaces (end-of-subRow padding for 3 deleted chars)
    REQUIRE(text[7] == U' '); REQUIRE(text[8] == U' '); REQUIRE(text[9] == U' ');
    // sub-row 1 completely unchanged
    REQUIRE(text.substr(10) == U"KLMNOPQRST");
}

// ---------------------------------------------------------------------------
// MainScreenDocument::SaveCursor / RestoreCursor (DECSC/DECRC — ESC 7 / ESC 8)
// ---------------------------------------------------------------------------

TEST_CASE("given cursor at col 5 when SaveCursor then RestoreCursor then cursor returns to col 5") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');
    doc.AppendInsertChar(U'B');
    doc.AppendInsertChar(U'C');
    doc.AppendInsertChar(U'D');
    doc.AppendInsertChar(U'E');
    REQUIRE(doc.GetCursor().col == 5);

    doc.SaveCursor();
    doc.MoveCursorLeft(3);
    REQUIRE(doc.GetCursor().col == 2);

    doc.RestoreCursor();
    REQUIRE(doc.GetCursor().col == 5);
    REQUIRE(doc.GetCursor().line == 0);
}

TEST_CASE("given no explicit SaveCursor when RestoreCursor then cursor moves to origin") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'X');
    doc.AppendInsertChar(U'Y');
    REQUIRE(doc.GetCursor().col == 2);

    doc.RestoreCursor();  // savedCursor_ default-initialised to {0, 0}
    REQUIRE(doc.GetCursor().col == 0);
    REQUIRE(doc.GetCursor().line == 0);
}

TEST_CASE("given save on line 1 when RestoreCursor then cursor line is restored") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'A');
    doc.NewLine();
    doc.AppendInsertChar(U'B');
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col == 1);

    doc.SaveCursor();
    doc.NewLine();
    doc.AppendInsertChar(U'C');
    REQUIRE(doc.GetCursor().line == 2);

    doc.RestoreCursor();
    REQUIRE(doc.GetCursor().line == 1);
    REQUIRE(doc.GetCursor().col == 1);
}

// ---------------------------------------------------------------------------
// CharWidth — Unicode East-Asian Width
// ---------------------------------------------------------------------------

TEST_CASE("given ASCII codepoint when CharWidth then returns 1") {
    REQUIRE(CharWidth(U'A')    == 1);
    REQUIRE(CharWidth(U' ')    == 1);
    REQUIRE(CharWidth(U'~')    == 1);
}

TEST_CASE("given fullwidth quotation mark U+FF02 when CharWidth then returns 2") {
    REQUIRE(CharWidth(U'＂') == 2);
}

TEST_CASE("given CJK unified ideograph when CharWidth then returns 2") {
    REQUIRE(CharWidth(U'中') == 2);  // 中
}

TEST_CASE("given combining diacritical mark when CharWidth then returns 0") {
    REQUIRE(CharWidth(U'́') == 0);  // combining acute accent
}

// ---------------------------------------------------------------------------
// Wide character handling in MainScreenDocument
// ---------------------------------------------------------------------------

TEST_CASE("given wide char when AppendInsertChar then cursor advances 2") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'＂');  // FULLWIDTH QUOTATION MARK

    REQUIRE(doc.GetCursor().col == 2);
}

TEST_CASE("given wide char when AppendInsertChar then filler placed at col+1") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'＂');

    const auto& text = doc.GetLines().back().text;
    REQUIRE(text.size() == 2);
    REQUIRE(text[0] == U'＂');
    REQUIRE(text[1] == kWideFiller);
}

TEST_CASE("given wide char followed by narrow when AppendInsertChar then narrow at correct col") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'＂');  // wide — cols 0–1
    doc.AppendInsertChar(U'A');       // narrow — col 2

    const auto& text = doc.GetLines().back().text;
    REQUIRE(text.size() == 3);
    REQUIRE(text[0] == U'＂');
    REQUIRE(text[1] == kWideFiller);
    REQUIRE(text[2] == U'A');
    REQUIRE(doc.GetCursor().col == 3);
}

TEST_CASE("given narrow wide narrow sequence when AppendInsertChar then layout correct") {
    MainScreenDocument doc;
    doc.AppendInsertChar(U'X');       // col 0
    doc.AppendInsertChar(U'中'); // wide CJK — cols 1–2
    doc.AppendInsertChar(U'Y');       // col 3

    const auto& text = doc.GetLines().back().text;
    REQUIRE(text.size() == 4);
    REQUIRE(text[0] == U'X');
    REQUIRE(text[1] == U'中');
    REQUIRE(text[2] == kWideFiller);
    REQUIRE(text[3] == U'Y');
    REQUIRE(doc.GetCursor().col == 4);
}

// ---------------------------------------------------------------------------
// Wide character handling in AltScreenDocument
// ---------------------------------------------------------------------------

TEST_CASE("given wide char in AltScreen when AppendInsertChar then cursor advances 2") {
    AltScreenDocument doc(5, 20);
    doc.AppendInsertChar(U'＂');

    REQUIRE(doc.GetCursor().col == 2);
}

TEST_CASE("given wide char in AltScreen when AppendInsertChar then filler at col+1") {
    AltScreenDocument doc(5, 20);
    doc.AppendInsertChar(U'＂');

    const auto& text = doc.GetLines().front().text;
    REQUIRE(text.size() >= 2);
    REQUIRE(text[0] == U'＂');
    REQUIRE(text[1] == kWideFiller);
}

TEST_CASE("given wide char at last AltScreen column when AppendInsertChar then pendingWrap set") {
    // cols=3: writing a 2-wide char at col 1 means newCol=3 >= cols → wrap
    AltScreenDocument doc(3, 3);
    doc.AppendInsertChar(U'A');        // col 0 → cursor at 1
    doc.AppendInsertChar(U'＂');   // wide: cols 1–2 → newCol=3 → pendingWrap

    // Next printable char should trigger a new line
    doc.AppendInsertChar(U'B');
    REQUIRE(doc.GetCursor().line == 1);
}

TEST_CASE("given save then maxLines trimmed when RestoreCursor then savedCursor line does not underflow") {
    // maxLines = 2 so that the third NewLine evicts the first line.
    MainScreenDocument doc{2};
    doc.AppendInsertChar(U'A');
    doc.SaveCursor();   // saved at line 0, col 1
    doc.NewLine();
    doc.AppendInsertChar(U'B');
    doc.NewLine();      // this push causes pop_front; savedCursor_.line decremented to 0
    doc.AppendInsertChar(U'C');

    // Must not crash or produce a negative (underflow) line index.
    doc.RestoreCursor();
    REQUIRE(doc.GetCursor().line == 0);
}

// ---------------------------------------------------------------------------
// SGR style persistence — terminal-global, survives line breaks and cursor
// moves (regression: style used to live per-DocLine and reset on every \n)
// ---------------------------------------------------------------------------

TEST_CASE("given red SGR active when NewLine then style persists onto the next line") {
    MainScreenDocument doc;
    const Style red{1, -1, false};
    doc.SetCurrentStyle(red);
    doc.AppendInsertChar(U'a');
    doc.NewLine();
    doc.AppendInsertChar(U'b');

    const auto& line1 = doc.GetLines()[1];
    REQUIRE(line1.text == U"b");
    REQUIRE(line1.styles.size() == 1);
    REQUIRE(line1.styles[0].style == red);
}

TEST_CASE("given red SGR active in AltScreen when NewLine then style persists onto the next row") {
    AltScreenDocument doc(3, 5);
    const Style red{1, -1, false};
    doc.SetCurrentStyle(red);
    doc.AppendInsertChar(U'a');
    doc.NewLine();
    doc.AppendInsertChar(U'b');

    const auto& row1 = doc.GetLines()[1];
    REQUIRE(row1.styles.size() == 1);
    REQUIRE(row1.styles[0].style == red);
}

namespace {
struct NullScreen : term::parser::IScreenTarget {};
} // namespace

TEST_CASE("given red SGR escape spanning a newline when parsed then the second line is red") {
    MainScreenDocument doc;
    NullScreen screen;
    term::parser::Parser parser(doc, screen);

    parser.Process("\x1b[31mred1\nred2\x1b[0m");

    REQUIRE(doc.GetLines().size() == 2);
    const auto& line1 = doc.GetLines()[1];
    REQUIRE(line1.text == U"red2");
    REQUIRE(line1.styles.size() == 1);
    REQUIRE(line1.styles[0].style.fg == 1);
}

TEST_CASE("given SGR reset between lines when parsed then only the styled line is colored") {
    MainScreenDocument doc;
    NullScreen screen;
    term::parser::Parser parser(doc, screen);

    parser.Process("\x1b[32mgreen\x1b[0m\nplain");

    REQUIRE(doc.GetLines().size() == 2);
    REQUIRE(doc.GetLines()[0].styles[0].style.fg == 2);
    const auto& line1 = doc.GetLines()[1];
    REQUIRE(line1.styles.size() == 1);
    REQUIRE(line1.styles[0].style == Style{});
}

TEST_CASE("given FullReset when called then accumulated SGR state is cleared") {
    MainScreenDocument doc;
    doc.SetCurrentStyle(Style{1, -1, true});
    doc.FullReset(true);
    doc.AppendInsertChar(U'x');

    const auto& line0 = doc.GetLines()[0];
    REQUIRE(line0.styles.size() == 1);
    REQUIRE(line0.styles[0].style == Style{});
}
