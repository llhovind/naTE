#pragma once
#include <functional>
#include <wx/colour.h>
#include <wx/panel.h>

// Common base for the small custom-drawn toggle widgets in the tile title bar
// (WrapControl, AltScrControl, X11Control).  Owns the shared glyph color pair,
// the click callback, and the OnLeftDown handler so subclasses only supply
// their state flag and OnPaint.
class TileIndicatorControl : public wxPanel
{
public:
    void SetClickCallback(std::function<void()> cb);
    void SetGlyphColours(wxColour active, wxColour inactive);

protected:
    explicit TileIndicatorControl(wxWindow* parent);

    wxColour              glyphActive_   { 253, 246, 227 };  // UiColors default: base07
    wxColour              glyphInactive_ { 101, 123, 131 };  // UiColors default: base03
    std::function<void()> clickCb_;

private:
    void OnLeftDown(wxMouseEvent&);
};
