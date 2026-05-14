#include "ui/TerminalTile.h"
#include "ui/TerminalPanel.h"
// #include "ui/TerminalActions.h"
#include <wx/menu.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <cmath>

wxDEFINE_EVENT(EVT_TERMINAL_ACTION, wxCommandEvent);

TerminalTile::TerminalTile(wxWindow *parent, const AppConfig &cfg,
                           unsigned short cols, const std::string &label)
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

    // Buttons to add to TitleBar
    wxBitmap wrapBmp(wxT("./src/ui/icons/text-wrap.png"), wxBITMAP_TYPE_PNG);
    // auto* wrapIcon = new wxStaticBitmap(titleBar_, wxID_ANY, wrapBmp);
    wxBitmapButton *wrapBtn = new wxBitmapButton(titleBar_, wxID_ANY, wrapBmp, wxDefaultPosition, wxSize(16, 16), wxBORDER_NONE);
    wrapBtn->Bind(wxEVT_BUTTON, &TerminalTile::OnWrapClick, this);

    wxBoxSizer *hSizer = new wxBoxSizer(wxHORIZONTAL);
    hSizer->Add(titleText_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    hSizer->AddStretchSpacer(1); // pushes icon to the right
    hSizer->Add(wrapBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    titleBar_->SetSizer(hSizer);
    titleBar_->Layout();

    // Route title bar events: activate on click, detect drag threshold for move.
    titleBar_->Bind(wxEVT_LEFT_DOWN, &TerminalTile::OnTitleDown, this);
    titleBar_->Bind(wxEVT_MOTION, &TerminalTile::OnTitleMotion, this);
    titleBar_->Bind(wxEVT_LEFT_UP, &TerminalTile::OnTitleUp, this);
    titleBar_->Bind(wxEVT_RIGHT_DOWN, &TerminalTile::OnTitleRightClick, this);
    titleText_->Bind(wxEVT_LEFT_DOWN, &TerminalTile::OnTitleDown, this);
    titleText_->Bind(wxEVT_MOTION, &TerminalTile::OnTitleMotion, this);
    titleText_->Bind(wxEVT_LEFT_UP, &TerminalTile::OnTitleUp, this);
    titleText_->Bind(wxEVT_RIGHT_DOWN, &TerminalTile::OnTitleRightClick, this);

    terminal_ = new TerminalPanel(this, cfg, cols);

    // Tile min-size = panel client area + title bar; TerminalGrid uses this to
    // position without resizing, preserving the cols × rows pixel invariant.
    const wxSize panelMin = terminal_->GetMinClientSize();
    SetMinSize({panelMin.x, panelMin.y + kTitleBarHeight});

    Bind(wxEVT_SIZE, &TerminalTile::OnSize, this);
}

void TerminalTile::OnSize(wxSizeEvent &evt)
{
    const wxSize sz = GetClientSize();
    titleBar_->SetSize(0, 0, sz.x, kTitleBarHeight);
    terminal_->SetSize(0, kTitleBarHeight, sz.x, sz.y - kTitleBarHeight);
    evt.Skip();
}

void TerminalTile::UpdateTitleBarColor()
{
    const wxColour &c = inBroadcast_ ? colBroadcast_
                        : isFocused_ ? colActive_
                                     : colInactive_;
    titleBar_->SetBackgroundColour(c);
    titleBar_->Refresh();
}

void TerminalTile::SetFocused(bool focused)
{
    isFocused_ = focused;
    UpdateTitleBarColor();
}

void TerminalTile::SetBroadcastActive(bool active)
{
    inBroadcast_ = active;
    UpdateTitleBarColor();
}

void TerminalTile::SetTileLabel(const wxString &label)
{
    titleText_->SetLabel(label);
}

void TerminalTile::EmitAction(TerminalAction action)
{
    auto* data = new TerminalActionData{
        action,
        tileSessionId_
    };

    wxCommandEvent evt(EVT_TERMINAL_ACTION);

    evt.SetClientData(data);

    ProcessWindowEvent(evt);
}

void TerminalTile::OnWrapClick(wxCommandEvent &evt)
{
    EmitAction(TerminalAction::ToggleWrap);
    // std::printf("Wrap button clicked\n");
    // std::fflush(stdout);
}

void TerminalTile::OnTitleDown(wxMouseEvent &evt)
{
    if (evt.ControlDown())
    {
        if (broadcastToggleCb_)
            broadcastToggleCb_();
        return;
    }
    if (terminal_)
        terminal_->SetFocus();
    if (activateCb_)
        activateCb_();
    dragAnchor_ = evt.GetEventObject()
                      ? static_cast<wxWindow *>(evt.GetEventObject())->ClientToScreen(evt.GetPosition())
                      : evt.GetPosition();
    dragPending_ = true;
    evt.Skip();
}

void TerminalTile::OnTitleMotion(wxMouseEvent &evt)
{
    if (!dragPending_ || !evt.LeftIsDown())
    {
        dragPending_ = false;
        evt.Skip();
        return;
    }
    const wxPoint cur = static_cast<wxWindow *>(evt.GetEventObject())
                            ->ClientToScreen(evt.GetPosition());
    const int dx = cur.x - dragAnchor_.x;
    const int dy = cur.y - dragAnchor_.y;
    if (std::abs(dx) > kDragThreshold || std::abs(dy) > kDragThreshold)
    {
        dragPending_ = false;
        if (dragStartCb_)
            dragStartCb_(tileSessionId_, dragAnchor_);
    }
    evt.Skip();
}

void TerminalTile::OnTitleUp(wxMouseEvent &evt)
{
    dragPending_ = false;
    evt.Skip();
}

void TerminalTile::OnTitleRightClick(wxMouseEvent &)
{
    if (!broadcastToggleCb_)
        return;
    wxMenu menu;
    const wxString label = inBroadcast_ ? "Remove from Broadcast"
                                        : "Add to Broadcast";
    menu.Append(wxID_ANY, label);
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent &)
              {
        if (broadcastToggleCb_) broadcastToggleCb_(); });
    PopupMenu(&menu);
}
