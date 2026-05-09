#include "ui/TerminalTile.h"
#include "ui/TerminalPanel.h"
#include <wx/stattext.h>
#include <wx/sizer.h>

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

    // Route title bar clicks: set wx focus AND directly call the activate
    // callback, since GTK may not fire wxEVT_SET_FOCUS on a wxPanel click.
    auto onTitleClick = [this](wxMouseEvent&) {
        if (terminal_) terminal_->SetFocus();
        if (activateCb_) activateCb_();
    };
    titleBar_->Bind(wxEVT_LEFT_DOWN, onTitleClick);
    titleText_->Bind(wxEVT_LEFT_DOWN, onTitleClick);

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
