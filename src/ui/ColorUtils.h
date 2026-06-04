#pragma once
#include "config/Color.h"
#include <wx/colour.h>

inline wxColour toWx(const Rgb& c) { return { c.r, c.g, c.b }; }
