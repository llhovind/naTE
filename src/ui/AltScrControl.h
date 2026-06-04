#pragma once
#include "ui/TileIndicatorControl.h"

// Custom-drawn toggle that indicates whether the session is in alt-screen mode.
// Draws a two-state glyph (corner-marker viewport frame) and fires a callback on click.
class AltScrControl : public TileIndicatorControl
{
public:
    explicit AltScrControl(wxWindow* parent);

    void SetAltScrActive(bool active);

private:
    void OnPaint(wxPaintEvent&);

    bool altScrActive_ = false;
};
