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

// One-line summary of a queue's state, e.g. "3 of 7 remaining - 4.2 MiB of
// 40.1 MiB". Shared by the panel's own header and the explorer's status bar,
// which shows it while the panel itself is hidden in Explore mode — two copies
// of this sentence would drift.
wxString DescribeTransferQueue(const term::fs::TransferQueue& queue);

// What a queue that has just gone idle actually did, for the one-shot message a
// window shows when the last job retires.
//
// Separate from DescribeTransferQueue because that one describes a queue at any
// moment and this one closes it off. A batch the user declined at the space
// warning drains exactly like one that ran, so a fixed "Transfers finished."
// over a list of cancelled rows reads as a claim that the files moved.
wxString DescribeTransferOutcome(const term::fs::TransferQueue& queue);

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
    // Called with the state the user asked for, not a toggle: the button reads
    // the queue for its own label, so the queue stays the single authority on
    // whether anything is paused.
    void SetOnPauseChanged(std::function<void(bool paused)> cb) { onPauseChanged_ = std::move(cb); }
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
    std::function<void(bool)>            onPauseChanged_;

    TransferJobListCtrl* list_       = nullptr;
    wxStaticText*        summary_    = nullptr;
    wxButton*            pauseBtn_   = nullptr;
    wxButton*            cancelBtn_  = nullptr;
    wxButton*            cancelAllBtn_ = nullptr;
    wxButton*            clearBtn_   = nullptr;
};

} // namespace ui
