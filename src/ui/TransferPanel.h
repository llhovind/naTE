#pragma once

#include "config/Config.h"
#include "fs/TransferQueue.h"

#include <functional>

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/panel.h>
#include <wx/stattext.h>

namespace ui {

class TransferJobListCtrl;

// The transfer queue's face: one row per job, with progress, and the controls
// to stop them.
//
// Holds no copy of the queue's state. It renders from the live queue at paint
// time and is told only that something changed, so a row can never display a
// job's previous progress.
class TransferPanel : public wxPanel {
public:
    TransferPanel(wxWindow* parent, const AppConfig& cfg,
                  const term::fs::TransferQueue& queue);

    void SetOnCancelJob(std::function<void(term::fs::JobId)> cb) { onCancelJob_ = std::move(cb); }
    void SetOnCancelAll(std::function<void()> cb) { onCancelAll_ = std::move(cb); }
    void SetOnClearFinished(std::function<void()> cb) { onClearFinished_ = std::move(cb); }

    // Re-reads the queue. Cheap enough to call on every job change.
    void RefreshFromQueue();

    void ApplyConfig(const AppConfig& cfg);

private:
    void UpdateSummary();

    const term::fs::TransferQueue& queue_;
    AppConfig                      cfg_;

    std::function<void(term::fs::JobId)> onCancelJob_;
    std::function<void()>                onCancelAll_;
    std::function<void()>                onClearFinished_;

    TransferJobListCtrl* list_       = nullptr;
    wxStaticText*        summary_    = nullptr;
    wxButton*            cancelBtn_  = nullptr;
    wxButton*            cancelAllBtn_ = nullptr;
    wxButton*            clearBtn_   = nullptr;
};

} // namespace ui
