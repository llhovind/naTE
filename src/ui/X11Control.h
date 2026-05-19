#pragma once

#include <functional>
#include <wx/panel.h>

// Custom-drawn toggle that shows whether X11 forwarding is active on the session.
// Draws a small "X" glyph inside a square frame; fires a callback on click.
// Mirrors AltScrControl in structure; carries no keyboard focus.
class X11Control : public wxPanel
{
public:
    explicit X11Control(wxWindow* parent);

    void SetClickCallback(std::function<void()> cb);
    void SetX11Active(bool active);
    bool IsX11Active() const { return x11Active_; }

private:
    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);

    bool                  x11Active_ = false;
    std::function<void()> clickCb_;
};
