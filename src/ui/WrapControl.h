#pragma once

#include <functional>
#include <wx/panel.h>

// Custom-drawn hit region that replaces the wrapBtn_ wxBitmapButton.
// Draws a two-state glyph (wrap off / wrap on) and fires a callback on click.
// Being a plain wxPanel it carries no keyboard focus, eliminating the
// focus-traversal issue of the old button widget.
class WrapControl : public wxPanel
{
public:
    explicit WrapControl(wxWindow* parent);

    void SetClickCallback(std::function<void()> cb);
    void SetWrapActive(bool active);

private:
    void OnPaint(wxPaintEvent&);
    void OnLeftDown(wxMouseEvent&);

    bool                  wrapActive_ = false;
    std::function<void()> clickCb_;
};
