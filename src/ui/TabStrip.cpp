#include "ui/TabStrip.h"
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <algorithm>
#include <cmath>

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
// Tab data management
// ---------------------------------------------------------------------------

void TabStrip::AddTab(const wxString& label)
{
    labels_.push_back(label);
    if (activeIdx_ < 0)
        activeIdx_ = 0;
    Refresh();
}

void TabStrip::RemoveTab(int index)
{
    if (index < 0 || index >= (int)labels_.size()) return;
    labels_.erase(labels_.begin() + index);

    if (labels_.empty()) {
        activeIdx_ = -1;
    } else {
        activeIdx_ = std::min(activeIdx_, (int)labels_.size() - 1);
        if (activeIdx_ < 0) activeIdx_ = 0;
    }
    Refresh();
}

void TabStrip::SetTabLabel(int index, const wxString& label)
{
    if (index >= 0 && index < (int)labels_.size()) {
        labels_[index] = label;
        Refresh();
    }
}


void TabStrip::SetActiveTab(int index)
{
    if (activeIdx_ != index) {
        activeIdx_ = index;
        Refresh();
    }
}

void TabStrip::SetBgColour(const wxColour& c)
{
    bgColour_ = c;
    Refresh();
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

TabStrip::TabGeom TabStrip::ComputeGeom() const
{
    const int w = GetClientSize().x;
    const int n = static_cast<int>(labels_.size());

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
    const TabGeom g = ComputeGeom();

    // Only consider the tab zone: [0, n * tabW).
    if (g.tabW <= 0 || x >= (int)labels_.size() * g.tabW) return -1;

    const int idx = x / g.tabW;
    if (idx < 0 || idx >= (int)labels_.size()) return -1;

    const int tabRight = (idx + 1) * g.tabW;
    closeHit = (x >= tabRight - kCloseW);
    return idx;
}

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------

void TabStrip::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    const wxSize sz = GetClientSize();

    dc.SetBackground(wxBrush(bgColour_));
    dc.Clear();

    const TabGeom g   = ComputeGeom();
    const wxFont  fnt = GetFont();
    dc.SetFont(fnt);
    const int charH = dc.GetCharHeight();
    const int ty    = (sz.y - charH) / 2;

    if (!labels_.empty()) {
        // Slightly lighter active-tab colour (brighten each channel by ~35).
        const wxColour activeTabBg(
            std::min(255, bgColour_.Red()   + 35),
            std::min(255, bgColour_.Green() + 35),
            std::min(255, bgColour_.Blue()  + 35));

        // Darker separator between tabs.
        const wxColour sepColour(
            std::max(0, bgColour_.Red()   - 20),
            std::max(0, bgColour_.Green() - 20),
            std::max(0, bgColour_.Blue()  - 20));

        for (int i = 0; i < (int)labels_.size(); ++i) {
            const int x = i * g.tabW;
            const wxRect tabRect(x, 0, g.tabW, sz.y);

            const bool inBroadcast = broadcastQueryCb_ && broadcastQueryCb_(i);

            // Tab background: broadcast takes priority; active gets a brightness bump.
            if (inBroadcast || i == activeIdx_) {
                wxColour tabBg = inBroadcast ? colBroadcast_ : activeTabBg;
                // Active broadcast tab: darken slightly so it reads as "selected among broadcast".
                if (inBroadcast && i == activeIdx_) {
                    tabBg = wxColour(
                        std::max(0, (int)colBroadcast_.Red()   - 30),
                        std::max(0, (int)colBroadcast_.Green() - 30),
                        std::max(0, (int)colBroadcast_.Blue()  - 30));
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
            wxString label       = labels_[i];
            if (dc.GetTextExtent(label).x > labelAreaW) {
                while (!label.empty() && dc.GetTextExtent(label + L"…").x > labelAreaW)
                    label.RemoveLast();
                label += L"…";
            }
            dc.SetTextForeground(*wxWHITE);
            dc.DrawText(label, x + 6, ty);

            // Close "×" — dimmer on inactive non-broadcast tabs.
            const wxColour closeCol = (i == activeIdx_ || inBroadcast)
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
    if (headerRightClickCb_) headerRightClickCb_();
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
        if (idx != activeIdx_) {
            if (selectedCb_) selectedCb_(idx);
        }
        dragTabIdx_  = idx;
        dragAnchor_  = ClientToScreen(evt.GetPosition());
        dragPending_ = true;
        CaptureMouse();
        return;
    }

    // Blank header area (right of "+" button): activate tile and arm header drag.
    if (headerActivateCb_) headerActivateCb_();
    dragAnchor_        = ClientToScreen(evt.GetPosition());
    headerDragPending_ = true;
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
        evt.Skip();
        return;
    }

    if (dragPending_) {
        const wxPoint cur = ClientToScreen(evt.GetPosition());
        const int dx = cur.x - dragAnchor_.x;
        const int dy = cur.y - dragAnchor_.y;
        if (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold) {
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
        if (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold) {
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
        if (HasCapture()) ReleaseMouse();
    }
    evt.Skip();
}
