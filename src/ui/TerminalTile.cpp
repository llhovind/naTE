#include "ui/TerminalTile.h"
#include "ui/TerminalPanel.h"
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <cmath>

TerminalTile::TerminalTile(wxWindow* parent, const AppConfig& cfg,
                           unsigned short cols, const std::string& label)
    : wxPanel(parent, wxID_ANY)
{
    // Title bar — plain panel so we can set background colour independently.
    titleBar_ = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                            wxSize(-1, kTitleBarHeight));
    titleBar_->SetBackgroundColour(colInactive_);

    titleText_ = new wxStaticText(titleBar_, wxID_ANY,
                                  wxString::FromUTF8(label),
                                  wxPoint(6, 4));
    titleText_->SetForegroundColour(*wxWHITE);

    // Route title bar events: activate on click, detect drag threshold for move.
    titleBar_->Bind(wxEVT_LEFT_DOWN, &TerminalTile::OnTitleDown,   this);
    titleBar_->Bind(wxEVT_MOTION,    &TerminalTile::OnTitleMotion, this);
    titleBar_->Bind(wxEVT_LEFT_UP,   &TerminalTile::OnTitleUp,     this);
    titleText_->Bind(wxEVT_LEFT_DOWN, &TerminalTile::OnTitleDown,   this);
    titleText_->Bind(wxEVT_MOTION,    &TerminalTile::OnTitleMotion, this);
    titleText_->Bind(wxEVT_LEFT_UP,   &TerminalTile::OnTitleUp,     this);

    terminal_ = new TerminalPanel(this, cfg, cols);

    // Tile min-size = panel client area + title bar; TerminalGrid uses this to
    // position without resizing, preserving the cols × rows pixel invariant.
    const wxSize panelMin = terminal_->GetMinClientSize();
    SetMinSize({ panelMin.x, panelMin.y + kTitleBarHeight });

    Bind(wxEVT_SIZE, &TerminalTile::OnSize, this);
}

void TerminalTile::OnSize(wxSizeEvent& evt)
{
    const wxSize sz = GetClientSize();
    titleBar_->SetSize(0, 0, sz.x, kTitleBarHeight);
    terminal_->SetSize(0, kTitleBarHeight, sz.x, sz.y - kTitleBarHeight);
    evt.Skip();
}

void TerminalTile::SetFocused(bool focused)
{
    titleBar_->SetBackgroundColour(focused ? colActive_ : colInactive_);
    titleBar_->Refresh();
}

void TerminalTile::SetTileLabel(const wxString& label)
{
    titleText_->SetLabel(label);
}

void TerminalTile::OnTitleDown(wxMouseEvent& evt)
{
    if (terminal_) terminal_->SetFocus();
    if (activateCb_) activateCb_();
    dragAnchor_  = evt.GetEventObject()
                       ? static_cast<wxWindow*>(evt.GetEventObject())->ClientToScreen(evt.GetPosition())
                       : evt.GetPosition();
    dragPending_ = true;
    evt.Skip();
}

void TerminalTile::OnTitleMotion(wxMouseEvent& evt)
{
    if (!dragPending_ || !evt.LeftIsDown()) {
        dragPending_ = false;
        evt.Skip();
        return;
    }
    const wxPoint cur = static_cast<wxWindow*>(evt.GetEventObject())
                            ->ClientToScreen(evt.GetPosition());
    const int dx = cur.x - dragAnchor_.x;
    const int dy = cur.y - dragAnchor_.y;
    if (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold) {
        dragPending_ = false;
        if (dragStartCb_) dragStartCb_(tileSessionId_, dragAnchor_);
    }
    evt.Skip();
}

void TerminalTile::OnTitleUp(wxMouseEvent& evt)
{
    dragPending_ = false;
    evt.Skip();
}
