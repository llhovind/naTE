#include "PortForwardControl.h"
#include "ui/ColorUtils.h"

#include <wx/dcclient.h>
#include <wx/dcbuffer.h>

PortForwardControl::PortForwardControl(wxWindow* parent)
    : TileIndicatorControl(parent)
{
    Hide();  // shown only for SSH sessions via TerminalTile::ShowPortForwardControl()
    Bind(wxEVT_PAINT, &PortForwardControl::OnPaint, this);
}

void PortForwardControl::SetStatus(const std::vector<term::transport::PortForwardStatus>& status)
{
    bool active  = false;
    bool error   = false;
    bool warning = false;
    for (const auto& s : status) {
        if (s.active)           active  = true;
        if (!s.error.empty())   error   = true;
        if (!s.warning.empty()) warning = true;
    }

    if (hasActive_ == active && hasError_ == error && hasWarning_ == warning) return;
    hasActive_  = active;
    hasError_   = error;
    hasWarning_ = warning;
    Refresh();
}

void PortForwardControl::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    const wxSize sz  = GetClientSize();
    const int    cx  = sz.x / 2;
    const int    midY = sz.y * 10 / 20;

    // --- Pass 1: cloud dome (1px, dimmed 60% toward background) ---
    const wxColour cloudCol = blendWx(glyphActive_,
                                      GetParent()->GetBackgroundColour(), 0.6);
    dc.SetPen(wxPen(cloudCol, 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    // CCW arc from right edge to left edge draws the upper semicircle (dome).
    dc.DrawArc(sz.x - 3, midY,   // start (right)
               3,         midY,   // end   (left)
               cx,        midY);  // centre

    // --- Pass 2: bidirectional arrows (2px, full glyph colour) ---
    // Priority: error (forwarding prohibited) → warning (target unreachable)
    //         → active (tunnels open) → inactive. Error/warning outrank active so
    //         a forward that is "up" but unusable still reads as a problem.
    const wxColour arrowCol = hasError_   ? wxColour(210, 70, 70)
                            : hasWarning_ ? wxColour(220, 150, 40)
                            : hasActive_  ? glyphActive_
                                          : glyphInactive_;
    dc.SetPen(wxPen(arrowCol, 2));

    // Left-pointing arrow at ~60% of height.
    const int ay1 = sz.y * 12 / 20;
    dc.DrawLine(sz.x - 3, ay1, 3, ay1);        // shaft
    dc.DrawLine(3, ay1, 6, ay1 - 2);            // head upper
    dc.DrawLine(3, ay1, 6, ay1 + 2);            // head lower

    // Right-pointing arrow at ~80% of height.
    const int ay2 = sz.y * 16 / 20;
    dc.DrawLine(3, ay2, sz.x - 3, ay2);         // shaft
    dc.DrawLine(sz.x - 3, ay2, sz.x - 6, ay2 - 2);  // head upper
    dc.DrawLine(sz.x - 3, ay2, sz.x - 6, ay2 + 2);  // head lower
}
