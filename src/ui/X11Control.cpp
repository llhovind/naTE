#include "X11Control.h"

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>

X11Control::X11Control(wxWindow* parent)
    : TileIndicatorControl(parent)
{
    Hide();  // shown only for SSH sessions via TerminalTile::ShowX11Control()
    Bind(wxEVT_PAINT, &X11Control::OnPaint, this);
}

void X11Control::SetX11Active(bool active)
{
    if (x11Active_ == active) return;
    x11Active_ = active;
    Refresh();
}

void X11Control::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    const wxSize sz = GetClientSize();
    const int    cx = sz.x / 2;
    const int    cy = sz.y / 2;

    const wxColour col = x11Active_ ? glyphActive_ : glyphInactive_;
    dc.SetPen(wxPen(col, 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);

    // Outer square frame — 13×11, same footprint as AltScrControl corner markers.
    const int left  = cx - 6;
    const int right = cx + 6;
    const int top   = cy - 5;
    const int bot   = cy + 5;
    dc.DrawRectangle(left, top, right - left + 1, bot - top + 1);

    // Interior "X" — two diagonal lines crossing the centre of the frame.
    const int pad = 3;
    dc.DrawLine(left + pad, top + pad, right - pad, bot - pad);
    dc.DrawLine(right - pad, top + pad, left + pad, bot - pad);
}

