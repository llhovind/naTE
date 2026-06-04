#pragma once
#include "ui/TileIndicatorControl.h"

// Custom-drawn toggle that shows whether X11 forwarding is active on the session.
// Draws a small "X" glyph inside a square frame; fires a callback on click.
class X11Control : public TileIndicatorControl
{
public:
    explicit X11Control(wxWindow* parent);

    void SetX11Active(bool active);
    bool IsX11Active() const { return x11Active_; }

private:
    void OnPaint(wxPaintEvent&);

    bool x11Active_ = false;
};
