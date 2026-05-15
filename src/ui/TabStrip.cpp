#include "ui/TabStrip.h"
#include "ui/ISessionDropTarget.h"
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>

static const wxColour kColBroadcast { 255, 140, 0 };

TabStrip::TabStrip(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT,        &TabStrip::OnPaint,      this);
    Bind(wxEVT_LEFT_DOWN,    &TabStrip::OnLeftDown,   this);
    Bind(wxEVT_RIGHT_DOWN,   &TabStrip::OnRightDown,  this);
    Bind(wxEVT_MIDDLE_DOWN,  &TabStrip::OnMiddleDown, this);
    Bind(wxEVT_MOTION,       &TabStrip::OnMotion,     this);
    Bind(wxEVT_LEFT_UP,      &TabStrip::OnLeftUp,     this);
    Bind(wxEVT_LEAVE_WINDOW, &TabStrip::OnMouseLeave, this);
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

TabStrip::TabGeom TabStrip::ComputeGeom() const
{
    const int w = GetClientSize().x;
    const int n = tabCountCb_ ? tabCountCb_() : 0;

    TabGeom g;
    g.plusW = kPlusW;

    if (n > 0) {
        const int avail = std::max(0, w - kPlusW);
        g.tabW  = std::clamp(avail / n, kMinTabW, kMaxTabW);
        // "+" button sits immediately after the last tab, not at the far right.
        g.plusX = n * g.tabW;
    } else {
        // No tabs — "+" anchored to the left edge.
        g.plusX = 0;
    }
    return g;
}

int TabStrip::HitTest(int x, bool& closeHit) const
{
    closeHit = false;
    const TabGeom g   = ComputeGeom();
    const int     n   = tabCountCb_ ? tabCountCb_() : 0;

    if (g.tabW <= 0 || x >= n * g.tabW) return -1;

    const int idx = x / g.tabW;
    if (idx < 0 || idx >= n) return -1;

    const int tabRight = (idx + 1) * g.tabW;
    closeHit = (x >= tabRight - kCloseW);
    return idx;
}

int TabStrip::DropIndexAt(int x) const
{
    const TabGeom g = ComputeGeom();
    const int     n = tabCountCb_ ? tabCountCb_() : 0;
    if (n == 0 || g.tabW <= 0) return 0;

    const int clamped     = std::clamp(x, 0, n * g.tabW);
    const int tabIdx      = std::min(clamped / g.tabW, n - 1);
    const int offsetInTab = clamped - tabIdx * g.tabW;
    return (offsetInTab < g.tabW / 2) ? tabIdx : tabIdx + 1;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void TabStrip::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize sz = GetClientSize();

    const wxColour bgColour = bgColourCb_ ? bgColourCb_() : wxColour(131, 136, 141);
    dc.SetBackground(wxBrush(bgColour));
    dc.Clear();

    const TabGeom g   = ComputeGeom();
    const int     n   = tabCountCb_ ? tabCountCb_() : 0;
    const int     activeIdx = activeTabCb_ ? activeTabCb_() : -1;
    const wxFont  fnt = GetFont();
    dc.SetFont(fnt);
    const int charH = dc.GetCharHeight();
    const int ty    = (sz.y - charH) / 2;

    if (n > 0) {
        // Slightly lighter active-tab colour (brighten each channel by ~35).
        const wxColour activeTabBg(
            std::min(255, bgColour.Red()   + 35),
            std::min(255, bgColour.Green() + 35),
            std::min(255, bgColour.Blue()  + 35));

        // Darker separator between tabs.
        const wxColour sepColour(
            std::max(0, bgColour.Red()   - 20),
            std::max(0, bgColour.Green() - 20),
            std::max(0, bgColour.Blue()  - 20));

        for (int i = 0; i < n; ++i) {
            const int x = i * g.tabW;
            const wxRect tabRect(x, 0, g.tabW, sz.y);

            const bool inBroadcast = broadcastQueryCb_ && broadcastQueryCb_(i);

            // Tab background: broadcast takes priority; active gets a brightness bump.
            if (inBroadcast || i == activeIdx) {
                wxColour tabBg = inBroadcast ? kColBroadcast : activeTabBg;
                // Active broadcast tab: darken slightly so it reads as "selected among broadcast".
                if (inBroadcast && i == activeIdx) {
                    tabBg = wxColour(
                        std::max(0, (int)kColBroadcast.Red()   - 30),
                        std::max(0, (int)kColBroadcast.Green() - 30),
                        std::max(0, (int)kColBroadcast.Blue()  - 30));
                }
                dc.SetBrush(wxBrush(tabBg));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(tabRect);
            }

            if (i > 0) {
                dc.SetPen(wxPen(sepColour));
                dc.DrawLine(x, 3, x, sz.y - 3);
            }

            // Label — truncated with ellipsis to fit label area.
            const int labelAreaW = g.tabW - kCloseW - 10;
            wxString label       = labelCb_ ? labelCb_(i) : wxString{};
            if (dc.GetTextExtent(label).x > labelAreaW) {
                while (!label.empty() && dc.GetTextExtent(label + L"…").x > labelAreaW)
                    label.RemoveLast();
                label += L"…";
            }
            dc.SetTextForeground(*wxWHITE);
            dc.DrawText(label, x + 6, ty);

            // Close "×" — dimmer on inactive non-broadcast tabs.
            const wxColour closeCol = (i == activeIdx || inBroadcast)
                ? wxColour(220, 220, 220)
                : wxColour(160, 160, 160);
            dc.SetTextForeground(closeCol);
            dc.DrawText(L"×", x + g.tabW - kCloseW + 2, ty);
        }
    }

    // "+" button — drawn at its precise slot.
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText("+", g.plusX + 6, ty);
}

// ---------------------------------------------------------------------------
// Mouse events
// ---------------------------------------------------------------------------

void TabStrip::OnRightDown(wxMouseEvent& evt)
{
    if (!headerRightClickCb_) return;
    bool closeHit = false;
    const int tabIdx = HitTest(evt.GetX(), closeHit);
    headerRightClickCb_(tabIdx);  // -1 = background; ≥0 = specific tab
    // Do not Skip — prevent titleBar_ from also receiving this click.
}

void TabStrip::OnLeftDown(wxMouseEvent& evt)
{
    // Ctrl+click anywhere in the strip toggles broadcast membership.
    if (evt.ControlDown()) {
        if (headerCtrlClickCb_) headerCtrlClickCb_();
        return;
    }

    const int x = evt.GetX();
    const TabGeom g = ComputeGeom();

    // Precise "+" button zone: [plusX, plusX + kPlusW).
    if (x >= g.plusX && x < g.plusX + kPlusW) {
        if (newTabCb_) newTabCb_();
        return;  // consumed — do not Skip; prevents titleBar_ from seeing this
    }

    bool closeHit = false;
    const int idx = HitTest(x, closeHit);

    if (idx >= 0) {
        // Click landed on a tab.
        if (closeHit) {
            if (closeCb_) closeCb_(idx);
            return;
        }
        const int activeIdx = activeTabCb_ ? activeTabCb_() : -1;
        if (idx != activeIdx) {
            if (selectedCb_) selectedCb_(idx);
        } else {
            // Tab is already active in this tile but the tile itself may be
            // inactive — fire activate so focus bubbles up to UIManager.
            if (headerActivateCb_) headerActivateCb_();
        }
        dragTabIdx_  = idx;
        dragAnchor_  = ClientToScreen(evt.GetPosition());
        dragPending_ = true;
        wxSetCursor(wxCursor(wxCURSOR_HAND));
        CaptureMouse();
        return;
    }

    // Blank header area (right of "+" button): activate tile and arm header drag.
    if (headerActivateCb_) headerActivateCb_();
    dragAnchor_        = ClientToScreen(evt.GetPosition());
    headerDragPending_ = true;
    wxSetCursor(wxCursor(wxCURSOR_HAND));
    CaptureMouse();
}

void TabStrip::OnMiddleDown(wxMouseEvent& evt)
{
    bool closeHit = false;
    const int idx = HitTest(evt.GetX(), closeHit);
    if (idx >= 0 && closeCb_)
        closeCb_(idx);
    evt.Skip();
}

void TabStrip::OnMotion(wxMouseEvent& evt)
{
    if (!evt.LeftIsDown()) {
        if (dragPending_ || headerDragPending_) {
            dragPending_       = false;
            headerDragPending_ = false;
            if (HasCapture()) ReleaseMouse();
        }

        bool closeHit  = false;
        const int idx  = HitTest(evt.GetX(), closeHit);
        if (idx != hoverTabIdx_) {
            hoverTabIdx_ = idx;
            if (idx >= 0 && labelCb_) {
                const wxString fullLabel = labelCb_(idx);
                const TabGeom  g         = ComputeGeom();
                const int      labelAreaW = g.tabW - kCloseW - 10;
                wxClientDC dc(this);
                dc.SetFont(GetFont());
                if (dc.GetTextExtent(fullLabel).x > labelAreaW)
                    SetToolTip(fullLabel);
                else
                    UnsetToolTip();
            } else {
                UnsetToolTip();
            }
        }

        evt.Skip();
        return;
    }

    if (dragPending_) {
        const wxPoint cur = ClientToScreen(evt.GetPosition());
        const int dx = cur.x - dragAnchor_.x;
        const int dy = cur.y - dragAnchor_.y;
        if (std::abs(dx) > ui::kDragThreshold || std::abs(dy) > ui::kDragThreshold) {
            dragPending_ = false;
            if (HasCapture()) ReleaseMouse();
            if (tabDragCb_) tabDragCb_(dragTabIdx_, dragAnchor_);
        }
        return;
    }

    if (headerDragPending_) {
        const wxPoint cur = ClientToScreen(evt.GetPosition());
        const int dx = cur.x - dragAnchor_.x;
        const int dy = cur.y - dragAnchor_.y;
        if (std::abs(dx) > ui::kDragThreshold || std::abs(dy) > ui::kDragThreshold) {
            headerDragPending_ = false;
            if (HasCapture()) ReleaseMouse();
            if (headerDragCb_) headerDragCb_(dragAnchor_);
        }
        return;
    }

    evt.Skip();
}

void TabStrip::OnLeftUp(wxMouseEvent& evt)
{
    if (dragPending_ || headerDragPending_)
        wxSetCursor(wxNullCursor);
    dragPending_       = false;
    headerDragPending_ = false;
    if (HasCapture()) ReleaseMouse();
    evt.Skip();
}

void TabStrip::OnMouseLeave(wxMouseEvent& evt)
{
    if (dragPending_ || headerDragPending_) {
        dragPending_       = false;
        headerDragPending_ = false;
        wxSetCursor(wxNullCursor);
        if (HasCapture()) ReleaseMouse();
    }
    hoverTabIdx_ = -1;
    UnsetToolTip();
    evt.Skip();
}
