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
