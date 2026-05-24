#pragma once

#include <wx/popupwin.h>
#include <wx/string.h>

namespace ui {

// A frameless top-level popup that tracks the cursor during a drag operation,
// giving the user a visual cue of what is being dragged.
class DragGhost final : public wxPopupWindow {
public:
    DragGhost(wxWindow* parent, const wxString& label);

    // Reposition the ghost so its top-left corner is offset from screenPt.
    void MoveTo(wxPoint screenPt);

private:
    void OnPaint(wxPaintEvent&);

    wxString label_;
};

} // namespace ui
