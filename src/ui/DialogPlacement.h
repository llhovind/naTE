#pragma once

#include <algorithm>

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

// Positions `dlg` centred over `anchor`'s on-screen rectangle, clamped to the
// display containing `anchor`.  No-op when `anchor` is null, in which case the
// dialog keeps its default placement.  Call after the dialog is fully built
// (its size must be valid) and before ShowModal().
void CentreDialogOnTile(wxDialog& dlg, wxWindow* anchor);

} // namespace ui
