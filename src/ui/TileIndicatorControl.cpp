#include "ui/TileIndicatorControl.h"

TileIndicatorControl::TileIndicatorControl(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize({20, 20});
    Bind(wxEVT_LEFT_DOWN, &TileIndicatorControl::OnLeftDown, this);
}

void TileIndicatorControl::SetClickCallback(std::function<void()> cb)
{
    clickCb_ = std::move(cb);
}

void TileIndicatorControl::SetGlyphColours(wxColour active, wxColour inactive)
{
    glyphActive_   = active;
    glyphInactive_ = inactive;
    Refresh();
}

void TileIndicatorControl::OnLeftDown(wxMouseEvent&)
{
    if (clickCb_) clickCb_();
}
