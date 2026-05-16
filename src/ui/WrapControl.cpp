#include "WrapControl.h"

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>

WrapControl::WrapControl(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(20, 20));

    Bind(wxEVT_PAINT,      &WrapControl::OnPaint,    this);
    Bind(wxEVT_LEFT_DOWN,  &WrapControl::OnLeftDown, this);
}

void WrapControl::SetClickCallback(std::function<void()> cb)
{
    clickCb_ = std::move(cb);
}

void WrapControl::SetWrapActive(bool active)
{
    if (wrapActive_ == active) return;
    wrapActive_ = active;
    Refresh();
}

void WrapControl::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);

    // Background — inherit from titleBar_ so we stay in sync with all three
    // colour states (inactive / active / broadcast) without extra coupling.
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    const wxSize sz = GetClientSize();
    const int    cx = sz.x / 2;
    const int    cy = sz.y / 2;

    // Glyph colour: white when wrap is on (prominent), dimmed when off.
    const wxColour glyphCol = wrapActive_ ? *wxWHITE : wxColour(160, 160, 160);
    dc.SetPen(wxPen(glyphCol, 1));

    if (wrapActive_) {
        // Shortened top line to make room for the return-arrow descender.
        dc.DrawLine(cx - 7, cy - 5, cx + 4, cy - 5);

        // Return arrow — vertical drop on the right, then arrowhead pointing left.
        // Descender extends 2px past the middle line so the arrowhead reads clearly
        // against it rather than merging with the mid-line.
        dc.DrawLine(cx + 7, cy - 5, cx + 7, cy + 4);  // descender
        dc.DrawLine(cx + 7, cy + 2, cx + 4, cy + 4);  // upper arrowhead arm
        dc.DrawLine(cx + 7, cy + 6, cx + 4, cy + 4);  // lower arrowhead arm
    } else {
        // Full-width top line (no wrap indicator).
        dc.DrawLine(cx - 7, cy - 5, cx + 7, cy - 5);
    }

    // Middle and short bottom lines are identical in both states.
    dc.DrawLine(cx - 7, cy,     cx + 7, cy);
    dc.DrawLine(cx - 7, cy + 5, cx - 1, cy + 5);
}

void WrapControl::OnLeftDown(wxMouseEvent&)
{
    if (clickCb_) clickCb_();
}
