#include "ui/SpaceWarningDialog.h"

#include "ui/ColorUtils.h"
#include "ui/StringUtils.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace ui {

namespace {

constexpr int kMargin   = 14;
constexpr int kMinWidth = 520;

// Columns of the "needs X, has Y" grid.
constexpr int kGridCols = 3;
constexpr int kGridGap  = 6;

// One row of the grid: which volume, what it was asked for, what it has.
void AddVolumeRow(wxWindow* parent, wxSizer* grid, const wxColour& fg,
                  const wxColour& alarm, const wxString& name,
                  const term::fs::VolumeForecast& volume)
{
    auto* label = new wxStaticText(parent, wxID_ANY, name);
    label->SetForegroundColour(fg);
    grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);

    auto* needs = new wxStaticText(
        parent, wxID_ANY,
        wxString::Format("needs %s", FormatByteSize(volume.requiredBytes)));
    needs->SetForegroundColour(fg);
    grid->Add(needs, 0, wxALIGN_CENTER_VERTICAL);

    // An unmeasured volume says so rather than showing a figure it does not
    // have. That is the ordinary state for an SFTP server without the statvfs
    // extension, and printing "0 available" there would be a lie about a disk
    // that may be entirely empty.
    auto* has = new wxStaticText(
        parent, wxID_ANY,
        volume.known
            ? wxString::Format("%s available", FormatByteSize(volume.availableBytes))
            : wxString("free space unknown"));
    has->SetForegroundColour(volume.Short() ? alarm : fg);
    grid->Add(has, 0, wxALIGN_CENTER_VERTICAL);
}

} // namespace

SpaceWarningDialog::SpaceWarningDialog(wxWindow* parent, const AppConfig& cfg,
                                       const term::fs::PreflightReport& report,
                                       const wxString& destinationLabel)
    : wxDialog(parent, wxID_ANY, "Before This Copy Starts",
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    const wxColour dialogBg = toWx(cfg.uiColors.frameBackground);
    const wxColour termBg   = toWx(cfg.ansiColors[0]);
    const wxColour termFg   = toWx(cfg.ansiColors[7]);
    const wxColour fg       = pickContrasting(dialogBg, termBg, termFg);
    // The palette's red, for the figure that is actually short.
    const wxColour alarm    = toWx(cfg.ansiColors[1]);

    SetBackgroundColour(dialogBg);
    SetForegroundColour(fg);

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // Read-only leads when it applies: no amount of free space makes a
    // read-only volume writable, so it is a different problem with a different
    // remedy and burying it under a size comparison would mislead.
    wxString headlineText = "This copy may not fit.";
    if (report.CopyWillBeIncomplete()) {
        // First of all of them. A copy that will be missing files is a
        // different and worse problem than one that might not fit: the user can
        // retry a transfer that ran out of room, but a partial copy reports
        // success and looks exactly like a complete one afterwards.
        headlineText = wxString::Format(
            "%zu director%s could not be read, so this copy will be incomplete.",
            report.unreadableDirectories,
            report.unreadableDirectories == 1 ? "y" : "ies");
    } else if (report.forecast.destinationReadOnly) {
        headlineText = wxString::Format("%s is mounted read-only.", destinationLabel);
    } else if (report.forecast.DestinationUnverifiable()) {
        // Not the same claim as "it will not fit", and it must not be dressed
        // up as one: nothing is known about this volume either way. What the
        // user is being told is that the usual check did not happen.
        headlineText = wxString::Format(
            "%s cannot report its free space, so this copy could not be checked.",
            destinationLabel);
    }

    auto* headline = new wxStaticText(this, wxID_ANY, headlineText);
    wxFont bold = headline->GetFont();
    bold.MakeBold();
    headline->SetFont(bold);
    headline->SetForegroundColour(fg);
    outer->Add(headline, 0, wxALL, kMargin);

    auto* grid = new wxFlexGridSizer(kGridCols, kGridGap, kGridGap * 3);
    AddVolumeRow(this, grid, fg, alarm, destinationLabel, report.forecast.destination);

    // Only when something in the batch actually passes through it. A staging
    // row reading "needs 0" on an ordinary upload would invite the user to
    // wonder what it is and why it is being mentioned.
    if (report.forecast.staging.requiredBytes > 0) {
        AddVolumeRow(this, grid, fg, alarm, "This computer (staging)",
                     report.forecast.staging);
    }
    outer->Add(grid, 0, wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    // The server's own words against the path it refused. Without them the
    // headline says only that something is wrong; with them the user can
    // usually tell at a glance whether it is a permission, a mount, or a
    // directory that has gone away underneath them.
    if (report.CopyWillBeIncomplete()) {
        auto* why = new wxStaticText(
            this, wxID_ANY,
            wxString::Format("First was '%s':\n%s",
                             DecodeForDisplay(report.firstFailurePath),
                             DecodeForDisplay(report.firstFailure.message)));
        why->SetForegroundColour(alarm);
        outer->Add(why, 0, wxLEFT | wxRIGHT | wxBOTTOM, kMargin);
    }

    // The staging figure needs explaining or it reads as a second full copy of
    // everything. It is one file's worth because the queue moves one file at a
    // time and reclaims each staging file as it goes.
    if (report.forecast.staging.Short()) {
        auto* note = new wxStaticText(
            this, wxID_ANY,
            "Copies between two servers pass through this computer one file at\n"
            "a time, so the staging figure is the largest single file, not the\n"
            "total. Setting TMPDIR to a larger volume moves that work there.");
        note->SetForegroundColour(fg);
        outer->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, kMargin);
    }

    auto* caveat = new wxStaticText(
        this, wxID_ANY,
        report.forecast.DestinationUnverifiable()
            ? "Reporting free space is an optional part of SFTP that this\n"
              "server does not provide, so there may well be room. Nothing\n"
              "here says there is not — only that it could not be confirmed."
            : "Free space is a snapshot and does not account for quotas, other\n"
              "writers, or filesystems that compress what they store.");
    caveat->SetForegroundColour(fg);
    outer->Add(caveat, 0, wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    auto* buttons     = new wxBoxSizer(wxHORIZONTAL);
    auto* proceedBtn  = new wxButton(this, wxID_ANY, "Copy Anyway");
    auto* cancelBtn   = new wxButton(this, wxID_ANY, "Cancel");

    const wxColour btnBg = toWx(cfg.uiColors.tileInactive);
    for (wxButton* b : {proceedBtn, cancelBtn}) {
        b->SetBackgroundColour(btnBg);
        b->SetForegroundColour(pickContrasting(btnBg, termBg, termFg));
    }

    buttons->AddStretchSpacer();
    buttons->Add(proceedBtn, 0, wxRIGHT, 6);
    buttons->Add(cancelBtn,  0);
    outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    proceedBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        proceed_ = true;
        EndModal(wxID_OK);
    });
    cancelBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EndModal(wxID_CANCEL);
    });

    // Cancel is the default, so Enter or Escape pressed without reading does
    // the reversible thing: nothing has been queued to disk yet, and a copy not
    // started is a copy that can still be started.
    cancelBtn->SetDefault();
    cancelBtn->SetFocus();

    SetSizerAndFit(outer);
    SetMinSize(wxSize(kMinWidth, GetSize().y));
}

} // namespace ui
