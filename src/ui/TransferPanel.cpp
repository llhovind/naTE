#include "ui/TransferPanel.h"

#include "fs/RemotePath.h"
#include "ui/ColorUtils.h"
#include "ui/StringUtils.h"
#include "ui/ToolButton.h"

#include <wx/sizer.h>

namespace ui {

namespace {

enum JobColumn { JobName = 0, JobRoute, JobProgress, JobStatus, JobColumnCount };

constexpr int kPanelHeight = 150;

// The pause button's two faces. Named because the button is built with one and
// relabelled with the other, and a literal in each place is how they drift.
constexpr const char* kPauseLabel  = "Pause";
constexpr const char* kResumeLabel = "Resume";

wxString DescribeState(const term::fs::TransferJob& job)
{
    using term::fs::JobState;
    switch (job.state) {
        case JobState::Queued:             return "Queued";
        case JobState::Checking:           return "Checking";
        case JobState::AwaitingResolution: return "Waiting for you";
        case JobState::Active:             return "Transferring";
        case JobState::Completed:          return "Done";
        case JobState::Skipped:            return "Skipped";
        case JobState::Cancelled:          return "Cancelled";
        case JobState::Failed:
            // The reason matters more than the word: a queue full of "Failed"
            // with no cause is not actionable.
            return "Failed: " + DecodeForDisplay(job.error.message);
    }
    return {};
}

wxString FormatProgress(const term::fs::TransferJob& job)
{
    if (job.totalBytes == 0) return job.transferredBytes ? FormatByteSize(job.transferredBytes)
                                                         : wxString("-");
    const int percent = static_cast<int>(
        (job.transferredBytes * 100) / job.totalBytes);
    return wxString::Format("%d%%  (%s / %s)", percent,
                            FormatByteSize(job.transferredBytes),
                            FormatByteSize(job.totalBytes));
}

} // namespace

// Rows are read from the live queue whenever wx asks for them.
class TransferJobListCtrl : public wxListCtrl {
public:
    TransferJobListCtrl(wxWindow* parent, const term::fs::TransferQueue& queue)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL)
        , queue_(queue)
    {}

protected:
    wxString OnGetItemText(long item, long column) const override
    {
        const auto& jobs = queue_.Jobs();
        if (item < 0 || static_cast<size_t>(item) >= jobs.size()) return {};
        const term::fs::TransferJob& job = jobs[static_cast<size_t>(item)];

        switch (column) {
            case JobName:
                return DecodeForDisplay(term::fs::path::Leaf(job.sourcePath));
            case JobRoute: {
                // Endpoints, not "upload"/"download": either side can now be
                // any session, and a server-to-server copy is neither.
                //
                // The arrow is built with FromUTF8 and concatenated rather
                // than embedded in a format string. Non-ASCII in a *format*
                // literal is worse than the usual mangling: wx converts the
                // format to wide characters up front, and a conversion failure
                // yields a null pointer that wxString::Format then walks.
                static const wxString kArrow =
                    wxString::FromUTF8("\xe2\x86\x92");   // U+2192 RIGHTWARDS ARROW
                wxString route = DecodeForDisplay(job.source.label) + " " +
                                 kArrow + " " +
                                 DecodeForDisplay(job.destination.label);
                if (job.viaLocalStaging) route += "  (via here)";
                return route;
            }
            case JobProgress: return FormatProgress(job);
            case JobStatus:   return DescribeState(job);
            default:          return {};
        }
    }

private:
    const term::fs::TransferQueue& queue_;
};

TransferPanel::TransferPanel(wxWindow* parent, const AppConfig& cfg,
                             const term::fs::TransferQueue& queue)
    : wxPanel(parent, wxID_ANY)
    , queue_(queue)
    , cfg_(cfg)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    summary_ = new wxStaticText(this, wxID_ANY, "No transfers.");
    // First of the three, and ahead of Cancel deliberately: it is the only one
    // that is not destructive, and it is what someone reaches for when a
    // transfer turns out to be going somewhere it should not. Cancel throws
    // away whatever the current file has moved; this does not.
    pauseBtn_     = new wxButton(this, wxID_ANY, kPauseLabel, wxDefaultPosition, wxDefaultSize);
    cancelBtn_    = new wxButton(this, wxID_ANY, "Cancel", wxDefaultPosition, wxDefaultSize);
    cancelAllBtn_ = new wxButton(this, wxID_ANY, "Cancel All", wxDefaultPosition, wxDefaultSize);
    clearBtn_     = new wxButton(this, wxID_ANY, "Clear Finished", wxDefaultPosition, wxDefaultSize);

    header->Add(summary_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    header->Add(pauseBtn_,     0, wxRIGHT, 4);
    header->Add(cancelBtn_,    0, wxRIGHT, 4);
    header->Add(cancelAllBtn_, 0, wxRIGHT, 4);
    header->Add(clearBtn_,     0);
    outer->Add(header, 0, wxEXPAND | wxALL, 6);

    list_ = new TransferJobListCtrl(this, queue_);
    list_->InsertColumn(JobName,      "File",      wxLIST_FORMAT_LEFT,  240);
    list_->InsertColumn(JobRoute,     "Route",     wxLIST_FORMAT_LEFT,  260);
    list_->InsertColumn(JobProgress,  "Progress",  wxLIST_FORMAT_LEFT,  200);
    list_->InsertColumn(JobStatus,    "Status",    wxLIST_FORMAT_LEFT,  260);
    outer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    SetSizer(outer);
    SetMinSize(wxSize(-1, kPanelHeight));

    cancelBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const long row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (row == wxNOT_FOUND) return;
        const auto& jobs = queue_.Jobs();
        if (static_cast<size_t>(row) >= jobs.size()) return;
        if (onCancelJob_) onCancelJob_(jobs[static_cast<size_t>(row)].id);
    });
    pauseBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // The queue's current state decides what the press means, rather than a
        // flag kept here: the queue can resume itself, and a remembered copy
        // would then have the button offering the state it is already in.
        if (onPauseChanged_) onPauseChanged_(!queue_.IsPaused());
    });
    cancelAllBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onCancelAll_) onCancelAll_();
    });
    clearBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (onClearFinished_) onClearFinished_();
    });

    ApplyConfig(cfg_);
    RefreshFromQueue();
}

void TransferPanel::RefreshFromQueue()
{
    const long count = static_cast<long>(queue_.Jobs().size());
    list_->SetItemCount(count);
    if (count > 0) list_->RefreshItems(0, count - 1);
    else           list_->Refresh();
    UpdateSummary();
}

namespace {

// What the pre-flight check weighed, and against what.
//
// Shown for every batch rather than only for the ones that do not fit. A check
// that speaks up solely when it objects is indistinguishable from one that
// never ran, or that measured the wrong thing and found it fits — and those are
// exactly the failures nobody can report because there is nothing on screen to
// report. Stating the two numbers makes the check accountable: a total that
// looks nothing like the selection is visible at a glance.
wxString DescribeForecast(const term::fs::TransferQueue& queue)
{
    const auto& report = queue.LastPreflight();
    if (!report) return {};

    const term::fs::SpaceForecast& forecast = report->forecast;

    wxString text = wxString::Format("  -  checked %s against ",
                                     FormatByteSize(forecast.destination.requiredBytes));

    text += forecast.destination.known
                ? wxString::Format("%s free",
                                   FormatByteSize(forecast.destination.availableBytes))
                : wxString("a destination that cannot report free space");

    if (forecast.staging.requiredBytes > 0 && forecast.staging.known) {
        text += wxString::Format(", staging %s against %s free",
                                 FormatByteSize(forecast.staging.requiredBytes),
                                 FormatByteSize(forecast.staging.availableBytes));
    }

    // Loud, and never omitted: a copy missing whole directories must not be
    // something the user has to open a dialog to find out about.
    if (report->CopyWillBeIncomplete()) {
        text += wxString::Format("  -  INCOMPLETE: %zu director%s could not be read",
                                 report->unreadableDirectories,
                                 report->unreadableDirectories == 1 ? "y" : "ies");
    }
    return text;
}

} // namespace

wxString DescribeTransferQueue(const term::fs::TransferQueue& queue)
{
    const size_t pending = queue.PendingCount();
    const size_t total   = queue.Jobs().size();

    // Ahead of everything else: while a walk is running there is nothing queued
    // and nothing moving, so every other sentence here would read "No
    // transfers" and the window would look asleep. On a large tree this state
    // can last a long time.
    if (queue.IsExpanding()) {
        return total == 0
                 ? wxString("Examining directories...")
                 : wxString::Format("Examining directories...  -  %zu file%s, %s so far",
                                    total, total == 1 ? "" : "s",
                                    FormatByteSize(queue.TotalBytes()));
    }

    // Ahead of the counts: a paused queue with work outstanding otherwise reads
    // exactly like one that is running, and the user is left watching a figure
    // that has stopped moving with no explanation for it.
    if (queue.IsPaused() && pending > 0) {
        return wxString::Format("Paused  -  %zu of %zu remaining  -  %s of %s",
                                pending, total,
                                FormatByteSize(queue.TransferredBytes()),
                                FormatByteSize(queue.TotalBytes()))
               + DescribeForecast(queue);
    }

    if (total == 0)
        return "No transfers.";
    if (pending == 0)
        return wxString::Format("%zu transfer%s finished  -  %s moved",
                                total, total == 1 ? "" : "s",
                                FormatByteSize(queue.TransferredBytes()));

    return wxString::Format("%zu of %zu remaining  -  %s of %s",
                            pending, total,
                            FormatByteSize(queue.TransferredBytes()),
                            FormatByteSize(queue.TotalBytes()))
           + DescribeForecast(queue);
}

void TransferPanel::UpdateSummary()
{
    const size_t pending = queue_.PendingCount();
    const size_t total   = queue_.Jobs().size();

    summary_->SetLabel(DescribeTransferQueue(queue_));

    cancelAllBtn_->Enable(pending > 0);
    cancelBtn_->Enable(pending > 0);
    clearBtn_->Enable(total > pending);

    // Offered while anything is still to come, including when the queue is
    // already paused — that is exactly when the button has to be reachable to
    // undo it. A paused queue with nothing left is a contradiction the user
    // should not have to resolve, so the label goes back to Pause with it.
    const bool paused = queue_.IsPaused() && pending > 0;
    pauseBtn_->Enable(pending > 0);
    if (pauseBtn_->GetLabel() != (paused ? kResumeLabel : kPauseLabel)) {
        pauseBtn_->SetLabel(paused ? kResumeLabel : kPauseLabel);
        // The glyph goes with the word. Re-applied rather than left alone
        // because relabelling a button that carries a bitmap is the operation
        // GTK is known to lose the bitmap on.
        StyleToolButton(pauseBtn_, paused ? Icon::Resume : Icon::Pause,
                        toWx(cfg_.uiColors.tileInactive),
                        pickContrasting(toWx(cfg_.uiColors.tileInactive),
                                        toWx(cfg_.ansiColors[0]),
                                        toWx(cfg_.ansiColors[7])));
        Layout();
    }
}

void TransferPanel::ApplyConfig(const AppConfig& cfg)
{
    cfg_ = cfg;

    const wxColour bg     = toWx(cfg_.ansiColors[0]);
    const wxColour fg     = toWx(cfg_.ansiColors[7]);
    const wxColour paneBg = toWx(cfg_.uiColors.frameBackground);

    SetBackgroundColour(paneBg);
    if (list_) {
        list_->SetBackgroundColour(bg);
        list_->SetForegroundColour(fg);
    }
    if (summary_) summary_->SetForegroundColour(pickContrasting(paneBg, bg, fg));

    // Words kept: all three act on a queue the user cannot get back, and
    // "Cancel" versus "Cancel All" is a distinction no pair of glyphs makes
    // safely at 16px. The icon is here to group them, not to replace the text.
    const wxColour btnBg = toWx(cfg_.uiColors.tileInactive);
    const wxColour btnFg = pickContrasting(btnBg, bg, fg);
    // Whichever face it is wearing: a theme change must not reset a paused
    // queue's button to "Pause" and leave it lying about the state.
    StyleToolButton(pauseBtn_,
                    queue_.IsPaused() ? Icon::Resume : Icon::Pause, btnBg, btnFg);
    StyleToolButton(cancelBtn_,    Icon::Cancel,        btnBg, btnFg);
    StyleToolButton(cancelAllBtn_, Icon::CancelAll,     btnBg, btnFg);
    StyleToolButton(clearBtn_,     Icon::ClearFinished, btnBg, btnFg);

    Layout();
    wxPanel::Refresh();
}

} // namespace ui
