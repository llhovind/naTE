#pragma once
#include "config/Color.h"
#include <wx/colour.h>

inline wxColour toWx(const Rgb& c) { return { c.r, c.g, c.b }; }

// Linear blend between two colors: t=0 → a, t=1 → b
inline wxColour blendWx(wxColour a, wxColour b, double t) {
    return { static_cast<unsigned char>(a.Red()   + (b.Red()   - a.Red())   * t),
             static_cast<unsigned char>(a.Green() + (b.Green() - a.Green()) * t),
             static_cast<unsigned char>(a.Blue()  + (b.Blue()  - a.Blue())  * t) };
}
