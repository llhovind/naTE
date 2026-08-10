#include <catch2/catch_test_macros.hpp>
#include "ui/PaneGeometry.h"

using ui::FrameWidthForPanes;
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
