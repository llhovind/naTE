#include <catch2/catch_test_macros.hpp>
#include "ui/PaneGeometry.h"

using ui::ControlsRowMetrics;
using ui::FrameWidthForPanes;
using ui::LeadingGapForSashAlignedPair;
using ui::MinFrameWidthForPanes;
using ui::PaneMetrics;

// A representative frame: 8px of sizer margin plus a hairline border, a 5px
// sash, and panes that may not go below 280.
static constexpr PaneMetrics kMetrics{ 10, 5, 280 };

TEST_CASE("given one pane when sizing the frame then it is that pane plus chrome")
{
    CHECK(FrameWidthForPanes(1, 700, kMetrics) == 710);
}

TEST_CASE("given two panes when sizing the frame then each pane keeps its width")
{
    // Two 700px panes: the second arrives at the size of the first, and the
    // sash is charged once.
    CHECK(FrameWidthForPanes(2, 700, kMetrics) == 10 + 700 + 5 + 700);
}

TEST_CASE("given a mode round trip when sizing the frame then the pane width is unchanged")
{
    const int explore = 710;

    // Explore -> Transfer: the width the frame grows to.
    const int paneWidth = explore - kMetrics.chrome;
    const int transfer  = FrameWidthForPanes(2, paneWidth, kMetrics);

    // Transfer -> Explore: the leading pane is still `paneWidth`, so the frame
    // returns to exactly the width it left. This is the property that makes
    // switching modes predictable; anything else drifts on every toggle.
    CHECK(FrameWidthForPanes(1, paneWidth, kMetrics) == explore);
    CHECK(transfer - explore == paneWidth + kMetrics.sash);
}

TEST_CASE("given repeated mode switches when sizing the frame then the width does not drift")
{
    int paneWidth = 640;
    for (int i = 0; i < 10; ++i) {
        const int transfer = FrameWidthForPanes(2, paneWidth, kMetrics);
        // Coming back, the leading pane is half of what the two panes shared.
        paneWidth = (transfer - kMetrics.chrome - kMetrics.sash) / 2;
    }
    CHECK(paneWidth == 640);
}

TEST_CASE("given a pane below the minimum when sizing the frame then the minimum is used")
{
    // A window can be doubled only from a width it was allowed to have.
    CHECK(FrameWidthForPanes(1, 100, kMetrics) == 10 + 280);
    CHECK(FrameWidthForPanes(2, 100, kMetrics) == 10 + 280 + 5 + 280);
}

TEST_CASE("given zero panes when sizing the frame then only the chrome remains")
{
    CHECK(FrameWidthForPanes(0, 700, kMetrics) == kMetrics.chrome);
}

TEST_CASE("given two panes when computing the minimum then it fits two whole panes")
{
    // 10 + 280 + 5 + 280 = 575, which exceeds the frame's own 400px floor.
    CHECK(MinFrameWidthForPanes(2, kMetrics, 400) == 575);
}

TEST_CASE("given one pane when computing the minimum then the frame floor wins")
{
    // One pane at its minimum is narrower than the window is usable at.
    CHECK(MinFrameWidthForPanes(1, kMetrics, 620) == 620);
}

// ---------------------------------------------------------------------------
// Copy buttons aligned to the sash
// ---------------------------------------------------------------------------

// A representative controls row in a 1000px-wide window: 6px margins, a mode
// button 120 wide with 16 after it, and two copy buttons whose labels name
// their destination and so differ in width.
static constexpr ControlsRowMetrics kRow{ 6, 994, 136, 150, 190, 8, 8 };

// Where the seam between the two buttons ends up for a given spacer.
static int Seam(int gap, ControlsRowMetrics m)
{
    return m.contentLeft + m.leading + gap + m.sourceButton + m.betweenButtons / 2;
}

TEST_CASE("given a centred sash when placing the copy pair then the seam is on the sash")
{
    const int sashCentre = 500;
    CHECK(Seam(LeadingGapForSashAlignedPair(sashCentre, kRow), kRow) == sashCentre);
}

TEST_CASE("given a dragged sash when placing the copy pair then the seam follows it")
{
    // The alignment is the sash's, not the window's: a sash the user pulled
    // left takes the buttons with it.
    for (const int sashCentre : {350, 400, 500, 640, 700})
        CHECK(Seam(LeadingGapForSashAlignedPair(sashCentre, kRow), kRow) == sashCentre);
}

TEST_CASE("given relabelled buttons when placing the copy pair then the seam holds")
{
    // Repointing a pane rewrites both labels, and each button changes width by
    // a different amount. The seam is what stays put.
    ControlsRowMetrics wider = kRow;
    wider.sourceButton = 260;
    wider.destButton   = 110;

    CHECK(Seam(LeadingGapForSashAlignedPair(500, wider), wider) == 500);
}

TEST_CASE("given a sash far left when placing the copy pair then the mode button is not overlapped")
{
    // The pair cannot reach a sash this far left without climbing over the
    // mode button, so it stops at its minimum distance from it.
    CHECK(LeadingGapForSashAlignedPair(100, kRow) == kRow.minGap);
}

TEST_CASE("given a sash far right when placing the copy pair then the pair stays in the row")
{
    const int gap = LeadingGapForSashAlignedPair(980, kRow);

    // Flush with the right edge of the row, not spilling past it.
    CHECK(kRow.contentLeft + kRow.leading + gap + kRow.sourceButton +
          kRow.betweenButtons + kRow.destButton == kRow.contentRight);
}

TEST_CASE("given a control pinned right when placing the pair then it is not overlapped")
{
    // The symlink choice sits at the right edge in Transfer mode. A sash far
    // right pulls the pair towards it, and the pair has to stop at the control
    // rather than slide underneath it.
    ControlsRowMetrics pinned = kRow;
    pinned.trailing = 140;

    const int gap = LeadingGapForSashAlignedPair(980, pinned);

    CHECK(pinned.contentLeft + pinned.leading + gap + pinned.sourceButton +
          pinned.betweenButtons + pinned.destButton ==
          pinned.contentRight - pinned.trailing);
}

TEST_CASE("given a control pinned right when the sash is reachable then the seam still lands on it")
{
    // The pinned control only bounds the pair; it must not push it off a sash
    // it can legally reach.
    ControlsRowMetrics pinned = kRow;
    pinned.trailing = 140;

    CHECK(Seam(LeadingGapForSashAlignedPair(500, pinned), pinned) == 500);
}

TEST_CASE("given a row too narrow for the pair when placing it then the gap closes entirely")
{
    // Long labels in a small window: there is no legal position, and the pair
    // gives up its spacer rather than being pushed off either end.
    ControlsRowMetrics cramped = kRow;
    cramped.contentRight = 400;

    CHECK(LeadingGapForSashAlignedPair(200, cramped) == 0);
}
