#pragma once

#include <string>
#include <wx/dialog.h>

namespace ui {

// Modal confirmation dialog shown when a multi-line paste is attempted without
// bracketed-paste protection. Displays the full paste text so the user can
// review it before deciding whether to proceed.
class PasteConfirmDialog : public wxDialog {
public:
    PasteConfirmDialog(wxWindow* parent, const std::string& utf8Text);
};

} // namespace ui
