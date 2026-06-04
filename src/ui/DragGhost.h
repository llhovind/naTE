#pragma once

#include <wx/colour.h>
#include <wx/popupwin.h>
#include <wx/string.h>

namespace ui {

// A frameless top-level popup that tracks the cursor during a drag operation,
// giving the user a visual cue of what is being dragged.
class DragGhost final : public wxPopupWindow {
public:
    // bg/fg default to the UiColors Solarized Dark tileInactive/tabText values
    // so callers that don't have a live AppConfig still look reasonable.
    DragGhost(wxWindow*       parent,
              const wxString& label,
              wxColour        bg = wxColour(101, 123, 131),
              wxColour        fg = wxColour(253, 246, 227));

    // Reposition the ghost so its top-left corner is offset from screenPt.
    void MoveTo(wxPoint screenPt);

private:
    void OnPaint(wxPaintEvent&);

    wxString label_;
    wxColour bg_;
    wxColour fg_;
};

} // namespace ui
