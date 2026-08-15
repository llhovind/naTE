#pragma once

#include "config/Config.h"
#include "fs/PreflightReport.h"

#include <wx/dialog.h>

namespace ui {

// Warns that a batch of transfers may not fit, before any of it starts.
//
// It warns rather than refuses. Every figure behind it is a snapshot that
// cannot see quotas, other writers, compression or thin provisioning, so the
// user is the one who knows whether it is really a problem — which is why
// Copy Anyway is a first-class button here and not a grudging escape hatch.
//
// Closing the window cancels. The batch has not started, so declining costs
// nothing and can be retried; proceeding into a volume that is genuinely full
// wastes however long the transfer takes to fail.
class SpaceWarningDialog : public wxDialog {
public:
    SpaceWarningDialog(wxWindow* parent, const AppConfig& cfg,
                       const term::fs::PreflightReport& report,
                       const wxString& destinationLabel);

    bool Proceed() const noexcept { return proceed_; }

private:
    bool proceed_ = false;
};

} // namespace ui
