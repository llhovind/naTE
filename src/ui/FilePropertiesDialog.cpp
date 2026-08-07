#include "ui/FilePropertiesDialog.h"

#include "fs/FileMode.h"
#include "ui/ColorUtils.h"
#include "ui/StringUtils.h"

#include <ctime>

#include <wx/app.h>
#include <wx/button.h>
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
    , alive_(std::make_shared<std::atomic<bool>>(true))
{
    const wxColour fg = toWx(cfg.uiColors.tabText);
    SetBackgroundColour(toWx(cfg.uiColors.frameBackground));
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
    AddRow(this, grid, "Permissions",
           wxString::Format("%s  (%s)",
                            wxString::FromUTF8(term::fs::FormatPermissions(info.mode)),
                            wxString::FromUTF8(term::fs::FormatOctal(info.mode))));
    AddRow(this, grid, "Owner",    FormatOwnership(info));
    AddRow(this, grid, "Modified", FormatTimestamp(info.mtime));

    if (info.isSymlink)
        linkTargetValue_ = AddRow(this, grid, "Link target", "Resolving...");

    outer->Add(grid, 1, wxEXPAND | wxALL, kMargin);

    auto* buttons = new wxStdDialogButtonSizer();
    auto* closeBtn = new wxButton(this, wxID_OK, "Close");
    closeBtn->SetBackgroundColour(toWx(cfg.uiColors.tileInactive));
    closeBtn->SetForegroundColour(fg);
    buttons->AddButton(closeBtn);
    buttons->Realize();
    // Realize() is what binds the affirmative button; SetDefault must follow it
    // or CreateStdDialogButtonSizer's wiring overrides the choice.
    closeBtn->SetDefault();
    outer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kMargin);

    SetSizerAndFit(outer);
    SetMinSize(wxSize(kMinWidth, GetSize().y));

    if (!info.isSymlink) return;

    std::weak_ptr<std::atomic<bool>> weakAlive = alive_;
    remote.ReadLink(fullPath, [this, weakAlive](std::string target,
                                                term::transport::FsError err) {
        wxTheApp->CallAfter([this, weakAlive, target = std::move(target),
                             err = std::move(err)]() mutable {
            const auto alive = weakAlive.lock();
            if (!alive || !alive->load(std::memory_order_acquire)) return;
            if (!linkTargetValue_) return;
            linkTargetValue_->SetLabel(err.Failed()
                                           ? "Unreadable: " + DecodeForDisplay(err.message)
                                           : DecodeForDisplay(target));
            Layout();
        });
    });
}

// The readlink continuation checks this before touching the dialog, so the
// dialog must be destroyed on the UI thread — which it is, being a wx window.
FilePropertiesDialog::~FilePropertiesDialog()
{
    alive_->store(false, std::memory_order_release);
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
