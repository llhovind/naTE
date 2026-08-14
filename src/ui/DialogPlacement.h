#pragma once

#include <algorithm>
#include <cstddef>

// wxDialog / wxWindow are forward-declared so the pure geometry below can be
// included by wx-free unit tests; only DialogPlacement.cpp pulls in wx headers.
class wxDialog;
class wxWindow;

namespace ui {

// Plain integer geometry mirroring wxRect/wxPoint, used so the placement math
// stays free of any wx dependency and can be unit-tested headlessly.
struct ScreenRect  { int x, y, w, h; };
struct ScreenPoint { int x, y; };

// Pure geometry: returns the top-left position at which a dialog of size
// (dlgW, dlgH) should open to be centred over `anchor`, clamped so it stays
// fully inside `work` (the display work area).  When the dialog is larger than
// the work area along an axis, it is pinned to the work-area origin on that
// axis rather than overflowing the top/left edge off-screen.
inline ScreenPoint ComputeCentredDialogPos(ScreenRect anchor, int dlgW, int dlgH,
                                           ScreenRect work)
{
    int x = anchor.x + (anchor.w - dlgW) / 2;
    int y = anchor.y + (anchor.h - dlgH) / 2;

    const int maxX = work.x + std::max(0, work.w - dlgW);
    const int maxY = work.y + std::max(0, work.h - dlgH);

    x = std::clamp(x, work.x, maxX);
    y = std::clamp(y, work.y, maxY);
    return { x, y };
}

// Overlap between two rectangles along each axis, never negative.
inline int OverlapX(ScreenRect a, ScreenRect b)
{
    return std::max(0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
}

inline int OverlapY(ScreenRect a, ScreenRect b)
{
    return std::max(0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
}

// True when a window at `frame` would land somewhere the user can actually
// reach it: at least `margin` pixels of it on one display, in both directions.
//
// This is the guard on restoring a window's saved position. Monitors get
// unplugged, laptops get undocked, and a window that was on a second screen
// yesterday has nowhere to go today — replaying the old coordinates then opens
// it entirely off-screen, where recovering it takes a window-manager
// keybinding most users do not know they have. A margin rather than a bare
// intersection test because a window overlapping by three pixels is off-screen
// for every practical purpose, and leaves no title bar to drag it back by.
//
// `displays` is a plain array so callers can pass however many they enumerated;
// an empty list means the query failed, not that every position is bad, so it
// answers true and leaves the decision to the caller's own fallback.
inline bool RectIsReachable(ScreenRect frame, const ScreenRect* displays,
                            std::size_t count, int margin)
{
    if (!displays || count == 0) return true;

    for (std::size_t i = 0; i < count; ++i)
        if (OverlapX(frame, displays[i]) >= margin &&
            OverlapY(frame, displays[i]) >= margin)
            return true;

    return false;
}

// Positions `dlg` centred over `anchor`'s on-screen rectangle, clamped to the
// display containing `anchor`.  No-op when `anchor` is null, in which case the
// dialog keeps its default placement.  Call after the dialog is fully built
// (its size must be valid) and before ShowModal().
void CentreDialogOnTile(wxDialog& dlg, wxWindow* anchor);

} // namespace ui
