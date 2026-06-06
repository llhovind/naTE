#pragma once
#include "ui/TileIndicatorControl.h"

// Custom-drawn hit region that replaces the wrapBtn_ wxBitmapButton.
// Draws a two-state glyph (wrap off / wrap on) and fires a callback on click.
// Being a plain wxPanel it carries no keyboard focus, eliminating the
// focus-traversal issue of the old button widget.
class WrapControl : public TileIndicatorControl
{
public:
    explicit WrapControl(wxWindow* parent);

    void SetWrapActive(bool active);

private:
    void OnPaint(wxPaintEvent&);

    bool wrapActive_ = false;
};
