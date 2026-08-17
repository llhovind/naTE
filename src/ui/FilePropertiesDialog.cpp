#include "ui/FilePropertiesDialog.h"

#include "fs/FileMode.h"
#include "ui/ColorUtils.h"
#include "ui/StringUtils.h"

#include <ctime>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/statbox.h>
#include <wx/sizer.h>

namespace ui {

namespace {

constexpr int kLabelGap  = 12;
constexpr int kRowGap    = 6;
constexpr int kMargin    = 14;
constexpr int kMinWidth  = 420;

wxString DescribeType(const term::transport::FileInfo& info)
{
    if (info.isSymlink) return "Symbolic link";
    if (info.isDir)     return "Directory";
    if (term::fs::IsRegularFile(info.mode)) return "Regular file";
    return "Special file";
}

wxString FormatTimestamp(int64_t unixSeconds)
{
    if (unixSeconds == 0) return "Unknown";
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    return wxString::FromUTF8(buf);
}

wxString FormatOwnership(const term::transport::FileInfo& info)
{
    // SFTP attributes carry numeric ids; names only arrive when the server
    // supplies an `ls -l` style long entry. Show whichever we actually have
    // rather than inventing a name we cannot resolve.
    if (info.owner.empty() && info.group.empty())
        return wxString::Format("%u:%u", info.uid, info.gid);
    return wxString::Format("%s:%s  (%u:%u)",
                            DecodeForDisplay(info.owner),
                            DecodeForDisplay(info.group),
                            info.uid, info.gid);
}

} // namespace

FilePropertiesDialog::FilePropertiesDialog(
    wxWindow* parent,
    const AppConfig& cfg,
    const term::transport::FileInfo& info,
    const std::string& fullPath,
    term::transport::IRemoteFileSystem& remote)
    : wxDialog(parent, wxID_ANY, "Properties",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    // Text colours are measured against the surface each control actually sits
    // on. uiColors.tabText is the tempting shortcut and the wrong one here: it
    // is defined to contrast tileInactive, and on a light palette it resolves
    // to a near-white that vanishes against this dialog's background.
    const wxColour dialogBg = toWx(cfg.uiColors.frameBackground);
    const wxColour termBg   = toWx(cfg.ansiColors[0]);
    const wxColour termFg   = toWx(cfg.ansiColors[7]);
    const wxColour fg       = pickContrasting(dialogBg, termBg, termFg);

    SetBackgroundColour(dialogBg);
    SetForegroundColour(fg);

    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* grid  = new wxFlexGridSizer(2, kRowGap, kLabelGap);
    grid->AddGrowableCol(1, 1);

    AddRow(this, grid, "Name",        DecodeForDisplay(info.name));
    AddRow(this, grid, "Path",        DecodeForDisplay(fullPath));
    AddRow(this, grid, "Type",        DescribeType(info));
    if (!info.isDir)
        AddRow(this, grid, "Size",
               wxString::Format("%llu bytes",
                                static_cast<unsigned long long>(info.size)));
    AddRow(this, grid, "Owner",    FormatOwnership(info));
    AddRow(this, grid, "Modified", FormatTimestamp(info.mtime));

    if (info.isSymlink)
        linkTargetValue_ = AddRow(this, grid, "Link target", "Resolving...");

    outer->Add(grid, 1, wxEXPAND | wxALL, kMargin);

    termBg_ = termBg;
    termFg_ = termFg;
    originalMode_ = info.mode & (term::fs::kPermissionMask |
                                 term::fs::kSpecialModeMask);
    selectedMode_ = originalMode_;
    BuildPermissionEditor(outer);

    auto* buttons  = new wxStdDialogButtonSizer();
    auto* okBtn    = new wxButton(this, wxID_OK, "OK");
    auto* cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
    const wxColour btnBg = toWx(cfg.uiColors.tileInactive);
    for (wxButton* b : {okBtn, cancelBtn}) {
        b->SetBackgroundColour(btnBg);
        b->SetForegroundColour(pickContrasting(btnBg, termBg, termFg));
    }
    buttons->AddButton(okBtn);
    buttons->AddButton(cancelBtn);
    buttons->Realize();
    // Realize() is what binds the affirmative button; SetDefault must follow it
    // or the sizer's own wiring overrides the choice.
    okBtn->SetDefault();
    outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(kMinWidth, GetSize().y));

    if (!info.isSymlink) return;

    // The guard retires this if the dialog closed first; being a wx window, it
    // is destroyed on the same thread the dispatcher posts to, which is what
    // makes the check sufficient.
    auto ctx = guard_.For(this);
    remote.ReadLink(fullPath, [ctx](std::string target,
                                    term::transport::FsError err) {
        ctx.Post([target = std::move(target), err = std::move(err)](
                     FilePropertiesDialog& dlg) mutable {
            if (!dlg.linkTargetValue_) return;
            dlg.linkTargetValue_->SetLabel(
                err.Failed() ? "Unreadable: " + DecodeForDisplay(err.message)
                             : DecodeForDisplay(target));
            dlg.Layout();
        });
    });
}

void FilePropertiesDialog::BuildPermissionEditor(wxSizer* outer)
{
    const wxColour fg = GetForegroundColour();

    auto* box = new wxStaticBoxSizer(wxVERTICAL, this, "Permissions");
    box->GetStaticBox()->SetForegroundColour(fg);

    auto* octalRow = new wxBoxSizer(wxHORIZONTAL);
    auto* octalLabel = new wxStaticText(this, wxID_ANY, "Octal:");
    octalLabel->SetForegroundColour(fg);
    octalCtrl_ = new wxTextCtrl(this, wxID_ANY,
                                wxString::FromUTF8(term::fs::FormatOctal(selectedMode_)));
    octalCtrl_->SetBackgroundColour(termBg_);
    octalCtrl_->SetForegroundColour(termFg_);
    octalRow->Add(octalLabel, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    octalRow->Add(octalCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(octalRow, 0, wxALL, kRowGap);

    // Three rows of read/write/execute, laid out the way chmod documents them.
    static constexpr const char* kWho[3]  = {"Owner", "Group", "Other"};
    static constexpr const char* kWhat[3] = {"Read", "Write", "Execute"};

    auto* gridSizer = new wxFlexGridSizer(4, kRowGap, kLabelGap);
    gridSizer->AddSpacer(0);
    for (const char* what : kWhat) {
        auto* header = new wxStaticText(this, wxID_ANY, what);
        header->SetForegroundColour(fg);
        gridSizer->Add(header, 0, wxALIGN_CENTER);
    }
    for (int who = 0; who < 3; ++who) {
        auto* label = new wxStaticText(this, wxID_ANY, kWho[who]);
        label->SetForegroundColour(fg);
        gridSizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        for (int what = 0; what < 3; ++what) {
            const int idx = who * 3 + what;
            permBoxes_[idx] = new wxCheckBox(this, wxID_ANY, "");
            gridSizer->Add(permBoxes_[idx], 0, wxALIGN_CENTER);
            permBoxes_[idx]->Bind(wxEVT_CHECKBOX,
                                  [this](wxCommandEvent&) { SyncFromCheckboxes(); });
        }
    }
    box->Add(gridSizer, 0, wxALL, kRowGap);
    outer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    octalCtrl_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { SyncFromOctal(); });
    PushModeToControls();
}

void FilePropertiesDialog::SyncFromOctal()
{
    if (syncing_) return;
    uint32_t parsed = 0;
    // Rejecting silently is deliberate: the field is mid-edit while the user
    // types, and blanking or correcting it under the cursor would fight them.
    if (!term::fs::ParseOctal(octalCtrl_->GetValue().ToStdString(), parsed)) return;

    selectedMode_ = parsed;
    syncing_ = true;
    for (int i = 0; i < 9; ++i)
        permBoxes_[i]->SetValue((selectedMode_ & (0400u >> i)) != 0);
    syncing_ = false;
}

void FilePropertiesDialog::SyncFromCheckboxes()
{
    if (syncing_) return;
    uint32_t bits = 0;
    for (int i = 0; i < 9; ++i)
        if (permBoxes_[i]->GetValue()) bits |= (0400u >> i);

    // Preserve setuid/setgid/sticky, which the grid does not expose but the
    // octal field can carry — a chmod through this dialog must not silently
    // clear them.
    selectedMode_ = (selectedMode_ & ~term::fs::kPermissionMask) | bits;

    syncing_ = true;
    octalCtrl_->ChangeValue(
        wxString::FromUTF8(term::fs::FormatOctal(selectedMode_)));
    syncing_ = false;
}

void FilePropertiesDialog::PushModeToControls()
{
    syncing_ = true;
    for (int i = 0; i < 9; ++i)
        permBoxes_[i]->SetValue((selectedMode_ & (0400u >> i)) != 0);
    octalCtrl_->ChangeValue(
        wxString::FromUTF8(term::fs::FormatOctal(selectedMode_)));
    syncing_ = false;
}

wxStaticText* FilePropertiesDialog::AddRow(wxWindow* parent, wxSizer* grid,
                                           const wxString& label,
                                           const wxString& value)
{
    auto* labelCtrl = new wxStaticText(parent, wxID_ANY, label + ":");
    auto* valueCtrl = new wxStaticText(parent, wxID_ANY, value);
    labelCtrl->SetForegroundColour(parent->GetForegroundColour());
    valueCtrl->SetForegroundColour(parent->GetForegroundColour());
    grid->Add(labelCtrl, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
    grid->Add(valueCtrl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    return valueCtrl;
}

} // namespace ui
