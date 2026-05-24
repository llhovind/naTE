#include "ui/DragGhost.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcscreen.h>

namespace {

constexpr int kPadX = 8;
constexpr int kPadY = 4;
// Match the inactive-tab background used by TabStrip.
const wxColour kBgColour { 131, 136, 141 };
// Cursor offset so the ghost doesn't cover the drop target under the pointer.
constexpr int kCursorOffsetX = 12;
constexpr int kCursorOffsetY = 12;

} // namespace

namespace ui {

DragGhost::DragGhost(wxWindow* parent, const wxString& label)
    : wxPopupWindow(parent, wxBORDER_NONE)
    , label_(label)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &DragGhost::OnPaint, this);

    wxScreenDC dc;
    dc.SetFont(GetFont());
    const wxSize textSz = dc.GetTextExtent(label_);
    SetClientSize(textSz.x + 2 * kPadX, textSz.y + 2 * kPadY);
}

void DragGhost::MoveTo(wxPoint screenPt)
{
    SetPosition(screenPt + wxPoint(kCursorOffsetX, kCursorOffsetY));
}

void DragGhost::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(kBgColour));
    dc.Clear();
    dc.SetFont(GetFont());
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText(label_, kPadX, kPadY);
}

} // namespace ui
