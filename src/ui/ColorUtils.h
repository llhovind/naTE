#pragma once
#include "config/Color.h"
#include <cmath>
#include <wx/colour.h>

inline wxColour toWx(const Rgb& c) { return { c.r, c.g, c.b }; }

// Linear blend between two colors: t=0 → a, t=1 → b
inline wxColour blendWx(wxColour a, wxColour b, double t) {
    return { static_cast<unsigned char>(a.Red()   + (b.Red()   - a.Red())   * t),
             static_cast<unsigned char>(a.Green() + (b.Green() - a.Green()) * t),
             static_cast<unsigned char>(a.Blue()  + (b.Blue()  - a.Blue())  * t) };
}

// Perceived brightness, ITU-R BT.601 weighting.
inline double luminanceWx(wxColour c) {
    return 0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue();
}

// Returns whichever of `a` or `b` sits further from `bg` in luminance — i.e.
// the more legible of two candidate text colors on that background.
//
// Prefer this over reading a foreground straight out of UiColors whenever the
// background is not the one that slot was defined against. Every UiColors text
// slot is derived for a *specific* background (tabText contrasts tileInactive,
// and so on), so borrowing one for a different surface can land near-invisible
// text — and does, on light palettes.
inline wxColour pickContrasting(wxColour bg, wxColour a, wxColour b) {
    const double target = luminanceWx(bg);
    return std::abs(luminanceWx(a) - target) >= std::abs(luminanceWx(b) - target)
               ? a : b;
}
