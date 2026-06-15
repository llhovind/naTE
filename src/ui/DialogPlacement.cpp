#include "ui/DialogPlacement.h"

#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/gdicmn.h>
#include <wx/window.h>

namespace ui {

void CentreDialogOnTile(wxDialog& dlg, wxWindow* anchor)
{
    if (!anchor) return;

    const wxRect anchorRect = anchor->GetScreenRect();
    const wxSize dlgSize    = dlg.GetSize();

    const int displayIdx = wxDisplay::GetFromWindow(anchor);
    const wxDisplay display(displayIdx == wxNOT_FOUND ? 0 : displayIdx);
    const wxRect work = display.GetClientArea();

    const ScreenPoint pos = ComputeCentredDialogPos(
        { anchorRect.x, anchorRect.y, anchorRect.width, anchorRect.height },
        dlgSize.x, dlgSize.y,
        { work.x, work.y, work.width, work.height });

    dlg.SetPosition(wxPoint(pos.x, pos.y));
}

} // namespace ui
