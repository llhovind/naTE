#include "ui/RemoteFileListCtrl.h"

#include "fs/FileMode.h"
#include "ui/StringUtils.h"

#include <algorithm>
#include <ctime>

namespace ui {

namespace {

wxString FormatTime(int64_t unixSeconds)
{
    if (unixSeconds == 0) return {};
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return wxString::FromUTF8(buf);
}

} // namespace

RemoteFileListCtrl::RemoteFileListCtrl(wxWindow* parent, ModelProvider provider,
                                       long style)
    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                 wxLC_REPORT | wxLC_VIRTUAL | style)
    , provider_(std::move(provider))
{}

void RemoteFileListCtrl::InsertStandardColumns(const ColumnWidths& widths)
{
    InsertColumn(FileColName,        "Name",        wxLIST_FORMAT_LEFT,  widths[FileColName]);
    InsertColumn(FileColSize,        "Size",        wxLIST_FORMAT_RIGHT, widths[FileColSize]);
    InsertColumn(FileColModified,    "Modified",    wxLIST_FORMAT_LEFT,  widths[FileColModified]);
    InsertColumn(FileColPermissions, "Permissions", wxLIST_FORMAT_LEFT,  widths[FileColPermissions]);
    InsertColumn(FileColOwner,       "Owner",       wxLIST_FORMAT_LEFT,  widths[FileColOwner]);
}

void RemoteFileListCtrl::ApplyColumnWidths(const ColumnWidths& widths)
{
    const int columns = std::min(GetColumnCount(), static_cast<int>(FileColumnCount));
    for (int i = 0; i < columns; ++i)
        SetColumnWidth(i, widths[static_cast<size_t>(i)]);
}

RemoteFileListCtrl::ColumnWidths RemoteFileListCtrl::CurrentColumnWidths() const
{
    ColumnWidths widths = kDefaultFileExplorerColumnWidths;
    // A view that inserted its own columns has fewer than the standard five;
    // those keep the default rather than reading past the end.
    const int columns = std::min(GetColumnCount(), static_cast<int>(FileColumnCount));
    for (int i = 0; i < columns; ++i) {
        const int width = GetColumnWidth(i);
        if (width > 0) widths[static_cast<size_t>(i)] = width;
    }
    return widths;
}

void RemoteFileListCtrl::SetBrokenLinkColour(const wxColour& colour)
{
    brokenAttr_.SetTextColour(colour);
    hasBrokenColour_ = colour.IsOk();
}

wxItemAttr* RemoteFileListCtrl::OnGetItemAttr(long item) const
{
    if (!hasBrokenColour_) return nullptr;

    const term::fs::DirModel* model = provider_ ? provider_() : nullptr;
    if (!model || item < 0) return nullptr;
    const auto row = static_cast<size_t>(item);
    if (row >= model->VisibleCount()) return nullptr;

    // nullptr means "draw this row like every other", which is what everything
    // that is not a link leading nowhere gets.
    return model->LinkTargetAt(row) == term::fs::LinkTarget::Broken
               ? &brokenAttr_
               : nullptr;
}

wxString RemoteFileListCtrl::OnGetItemText(long item, long column) const
{
    const term::fs::DirModel* model = provider_ ? provider_() : nullptr;
    if (!model || item < 0) return {};
    const auto row = static_cast<size_t>(item);
    if (row >= model->VisibleCount()) return {};

    const term::transport::FileInfo& e = model->At(row);
    // A link known to lead to a directory is presented as one: it is ordered
    // with them and opens like them, so a row that looked like a file would be
    // the odd one out.
    const bool directoryLike = model->IsDirectoryLike(row);
    switch (column) {
        case FileColName:
            // Remote names are opaque bytes; a non-UTF-8 name must stay
            // visible rather than silently rendering as empty.
            return DecodeForDisplay(directoryLike ? e.name + "/" : e.name);
        case FileColSize:
            return directoryLike ? wxString("-") : FormatByteSize(e.size);
        case FileColModified:
            return FormatTime(e.mtime);
        case FileColPermissions:
            return wxString::FromUTF8(term::fs::FormatPermissions(e.mode));
        case FileColOwner:
            return DecodeForDisplay(e.group.empty() ? e.owner
                                                    : e.owner + ":" + e.group);
        default:
            return {};
    }
}

} // namespace ui
