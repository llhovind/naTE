#include <catch2/catch_test_macros.hpp>
#include "ui/DialogPlacement.h"

using ui::ComputeCentredDialogPos;
using ui::ScreenRect;

// A roomy 1920x1080 work area anchored at the origin for the common cases.
static constexpr ScreenRect kWork{ 0, 0, 1920, 1080 };

TEST_CASE("given anchor in mid-screen when centring then dialog is centred over anchor")
{
    // Anchor: 400x300 tile at (500,400).  Dialog: 200x100.
    const auto p = ComputeCentredDialogPos({ 500, 400, 400, 300 }, 200, 100, kWork);
    CHECK(p.x == 500 + (400 - 200) / 2);  // 600
    CHECK(p.y == 400 + (300 - 100) / 2);  // 500
}

TEST_CASE("given anchor against right/bottom edge when centring then dialog is clamped on-screen")
{
    // Anchor hugs the bottom-right corner; naive centre would spill off-screen.
    const auto p = ComputeCentredDialogPos({ 1840, 1020, 80, 60 }, 400, 300, kWork);
    CHECK(p.x == kWork.w - 400);  // 1520 — right edge flush
    CHECK(p.y == kWork.h - 300);  // 780  — bottom edge flush
    CHECK(p.x + 400 <= kWork.w);
    CHECK(p.y + 300 <= kWork.h);
}

TEST_CASE("given anchor against top/left edge when centring then dialog stays within work area")
{
    // Small anchor in the top-left; naive centre would go negative.
    const auto p = ComputeCentredDialogPos({ 0, 0, 40, 30 }, 300, 200, kWork);
    CHECK(p.x == 0);
    CHECK(p.y == 0);
}

TEST_CASE("given dialog larger than work area when centring then dialog pins to work-area origin")
{
    const auto p = ComputeCentredDialogPos({ 100, 100, 200, 200 }, 3000, 2000, kWork);
    CHECK(p.x == kWork.x);
    CHECK(p.y == kWork.y);
}

TEST_CASE("given work area with non-zero origin when centring then clamp respects that origin")
{
    // Second monitor to the right: work area starts at x=1920.
    const ScreenRect work{ 1920, 0, 1920, 1080 };
    // Anchor near that monitor's right edge.
    const auto p = ComputeCentredDialogPos({ 3800, 20, 40, 30 }, 400, 300, work);
    CHECK(p.x == work.x + (work.w - 400));  // 3440
    CHECK(p.y == 0);
    CHECK(p.x >= work.x);
}

// ---------------------------------------------------------------------------
// Restoring a window's saved position
// ---------------------------------------------------------------------------

using ui::RectIsReachable;

namespace {

// Two displays side by side, the second one to the right of the first — the
// arrangement a saved position most often outlives.
constexpr ScreenRect kDisplays[2] = { { 0, 0, 1920, 1080 },
                                      { 1920, 0, 1920, 1080 } };
constexpr ScreenRect kOneDisplay[1] = { { 0, 0, 1920, 1080 } };

// A title bar's worth, matching what FileExplorerManager asks for.
constexpr int kMargin = 80;

bool Reachable(ScreenRect frame, const ScreenRect* displays, std::size_t count)
{
    return RectIsReachable(frame, displays, count, kMargin);
}

} // namespace

TEST_CASE("given a window well inside a display when tested then the position is reachable")
{
    CHECK(Reachable({ 300, 200, 720, 700 }, kOneDisplay, 1));
}

TEST_CASE("given a window on a second display when that display is gone then the position is refused")
{
    // Saved while docked at (2400,300); the external monitor has since been
    // unplugged, so nothing of the window would be on the remaining screen.
    const ScreenRect saved{ 2400, 300, 720, 700 };

    CHECK(Reachable(saved, kDisplays, 2));
    CHECK_FALSE(Reachable(saved, kOneDisplay, 1));
}

TEST_CASE("given a window overlapping a display by less than the margin then the position is refused")
{
    // 40px of the window is on screen: visible, but with no title bar to grab.
    CHECK_FALSE(Reachable({ 1880, 400, 720, 700 }, kOneDisplay, 1));
}

TEST_CASE("given a window overlapping a display by exactly the margin then the position is accepted")
{
    // The boundary belongs to the reachable side: exactly a title bar's worth
    // is enough to drag the window back with.
    CHECK(Reachable({ 1920 - kMargin, 400, 720, 700 }, kOneDisplay, 1));
}

TEST_CASE("given a window off the top of a display when tested then the position is refused")
{
    // Horizontally fine, vertically above the screen — the case that hides the
    // title bar specifically, which is the one that cannot be recovered from.
    CHECK_FALSE(Reachable({ 400, -680, 720, 700 }, kOneDisplay, 1));
}

TEST_CASE("given a display left of the origin when a window sits on it then the position is reachable")
{
    // A monitor arranged to the left of the primary one has negative
    // coordinates throughout; they are positions, not error values.
    constexpr ScreenRect leftOfPrimary[2] = { { -1920, 0, 1920, 1080 },
                                              { 0, 0, 1920, 1080 } };

    CHECK(Reachable({ -1500, 200, 720, 700 }, leftOfPrimary, 2));
}

TEST_CASE("given no displays reported when tested then the position is accepted")
{
    // An empty list means the query failed, not that every position is bad.
    // Discarding a good saved position over a failed lookup would be worse
    // than trusting it.
    CHECK(Reachable({ 300, 200, 720, 700 }, nullptr, 0));
}
