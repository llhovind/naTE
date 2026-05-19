#include "AltScrControl.h"

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>

AltScrControl::AltScrControl(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(20, 20));

    Bind(wxEVT_PAINT,     &AltScrControl::OnPaint,    this);
    Bind(wxEVT_LEFT_DOWN, &AltScrControl::OnLeftDown, this);
}

void AltScrControl::SetClickCallback(std::function<void()> cb)
{
    clickCb_ = std::move(cb);
}

void AltScrControl::SetAltScrActive(bool active)
{
    if (altScrActive_ == active) return;
    altScrActive_ = active;
    Refresh();
}

void AltScrControl::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    // Inherit background colour from the title bar so the icon stays in sync
    // with all three colour states (inactive / active / broadcast).
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    const wxSize sz = GetClientSize();
    const int    cx = sz.x / 2;
    const int    cy = sz.y / 2;

    const wxColour col = altScrActive_ ? *wxWHITE : wxColour(160, 160, 160);
    dc.SetPen(wxPen(col, 1));

    // Four corner markers — L-shaped ticks at each corner of a 13×11 rectangle.
    // This reads as a "viewport frame" or "fixed-grid screen", distinguishing it
    // clearly from the WrapControl's line-based glyph.
    const int left  = cx - 6;
    const int right = cx + 6;
    const int top   = cy - 5;
    const int bot   = cy + 5;
    const int arm   = 3;

    // Top-left
    dc.DrawLine(left,       top, left + arm, top);
    dc.DrawLine(left,       top, left,       top + arm);
    // Top-right
    dc.DrawLine(right,      top, right - arm, top);
    dc.DrawLine(right,      top, right,       top + arm);
    // Bottom-left
    dc.DrawLine(left,       bot, left + arm, bot);
    dc.DrawLine(left,       bot, left,       bot - arm);
    // Bottom-right
    dc.DrawLine(right,      bot, right - arm, bot);
    dc.DrawLine(right,      bot, right,       bot - arm);

    if (altScrActive_) {
        // Cursor indicator: a small two-pixel bar near the top-left interior,
        // signalling "cursor-addressable screen".
        dc.DrawLine(left + 2, top + 2, left + 5, top + 2);
        dc.DrawLine(left + 2, top + 3, left + 5, top + 3);
    }
}

void AltScrControl::OnLeftDown(wxMouseEvent&)
{
    if (clickCb_) clickCb_();
}
