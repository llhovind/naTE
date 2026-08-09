#include "ui/ConflictDialog.h"

#include "fs/RemotePath.h"
#include "ui/ColorUtils.h"
#include "ui/StringUtils.h"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace ui {

namespace {
constexpr int kMargin   = 14;
constexpr int kMinWidth = 460;
} // namespace

ConflictDialog::ConflictDialog(wxWindow* parent, const AppConfig& cfg,
                               const term::fs::TransferJob& job)
    : wxDialog(parent, wxID_ANY, "File Already Exists",
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    const wxColour dialogBg = toWx(cfg.uiColors.frameBackground);
    const wxColour termBg   = toWx(cfg.ansiColors[0]);
    const wxColour termFg   = toWx(cfg.ansiColors[7]);
    const wxColour fg       = pickContrasting(dialogBg, termBg, termFg);

    SetBackgroundColour(dialogBg);
    SetForegroundColour(fg);

    auto* outer = new wxBoxSizer(wxVERTICAL);

    const wxString leaf = DecodeForDisplay(term::fs::path::Leaf(job.destPath));
    auto* headline = new wxStaticText(
        this, wxID_ANY, wxString::Format("'%s' already exists.", leaf));
    wxFont bold = headline->GetFont();
    bold.MakeBold();
    headline->SetFont(bold);
    headline->SetForegroundColour(fg);
    outer->Add(headline, 0, wxALL, kMargin);

    auto* detail = new wxStaticText(
        this, wxID_ANY,
        wxString::Format("Destination:  %s", DecodeForDisplay(job.destPath)));
    detail->SetForegroundColour(fg);
    outer->Add(detail, 0, wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    applyAll_ = new wxCheckBox(this, wxID_ANY,
                               "Apply this choice to the rest of the transfer");
    applyAll_->SetForegroundColour(fg);
    outer->Add(applyAll_, 0, wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* overwriteBtn = new wxButton(this, wxID_ANY, "Overwrite");
    auto* renameBtn    = new wxButton(this, wxID_ANY, "Keep Both");
    auto* skipBtn      = new wxButton(this, wxID_ANY, "Skip");

    const wxColour btnBg = toWx(cfg.uiColors.tileInactive);
    for (wxButton* b : {overwriteBtn, renameBtn, skipBtn}) {
        b->SetBackgroundColour(btnBg);
        b->SetForegroundColour(pickContrasting(btnBg, termBg, termFg));
    }

    buttons->AddStretchSpacer();
    buttons->Add(overwriteBtn, 0, wxRIGHT, 6);
    buttons->Add(renameBtn,    0, wxRIGHT, 6);
    buttons->Add(skipBtn,      0);
    outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    overwriteBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        Choose(term::fs::ConflictResolution::Overwrite);
    });
    renameBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        Choose(term::fs::ConflictResolution::Rename);
    });
    skipBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        Choose(term::fs::ConflictResolution::Skip);
    });

    // Skip is the default so that Enter or Escape, pressed without reading,
    // leaves the existing file alone.
    skipBtn->SetDefault();
    skipBtn->SetFocus();

    SetSizerAndFit(outer);
    SetMinSize(wxSize(kMinWidth, GetSize().y));
}

bool ConflictDialog::ApplyToAll() const
{
    return applyAll_ && applyAll_->GetValue();
}

void ConflictDialog::Choose(term::fs::ConflictResolution resolution)
{
    resolution_ = resolution;
    EndModal(wxID_OK);
}

} // namespace ui
