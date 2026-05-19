#pragma once

#include <functional>
#include <wx/panel.h>

// Custom-drawn toggle that indicates whether the session is in alt-screen mode.
// Draws a two-state glyph (corner-marker viewport frame) and fires a callback on click.
// Mirrors WrapControl in structure; carries no keyboard focus.
class AltScrControl : public wxPanel
{
public:
    explicit AltScrControl(wxWindow* parent);

    void SetClickCallback(std::function<void()> cb);
    void SetAltScrActive(bool active);

private:
    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);

    bool                  altScrActive_ = false;
    std::function<void()> clickCb_;
};
