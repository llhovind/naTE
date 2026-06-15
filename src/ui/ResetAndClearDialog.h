#pragma once

#include <wx/dialog.h>

namespace ui {

// Confirmation shown before Reset-and-Clear when the session has scrollback to
// lose.  Replaces a native wxMessageBox so the dialog can be positioned over
// the originating tile (native GTK message dialogs ignore SetPosition).
//
// ShowModal() returns:
//   wxID_YES    — save scrollback to a file, then clear
//   wxID_NO     — clear without saving
//   wxID_CANCEL — abort, leave the terminal untouched
class ResetAndClearDialog : public wxDialog {
public:
    explicit ResetAndClearDialog(wxWindow* parent);
};

} // namespace ui
