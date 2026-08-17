#pragma once

#include "config/Config.h"
#include "fs/TransferQueue.h"

#include <wx/checkbox.h>
#include <wx/dialog.h>

namespace ui {

// Asks what to do about a transfer whose destination already exists.
//
// There is no Cancel: every path out of this dialog has to be one of the
// three resolutions the queue understands, and closing the window resolves as
// Skip — the only choice that cannot destroy data.
class ConflictDialog : public wxDialog {
public:
    ConflictDialog(wxWindow* parent, const AppConfig& cfg,
                   const term::fs::TransferJob& job);

    term::fs::ConflictResolution Resolution() const noexcept { return resolution_; }
    bool ApplyToAll() const;

private:
    void Choose(term::fs::ConflictResolution resolution);

    term::fs::ConflictResolution resolution_ = term::fs::ConflictResolution::Skip;
    wxCheckBox* applyAll_ = nullptr;
};

} // namespace ui
