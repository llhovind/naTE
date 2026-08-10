#include "ui/RemoteFileListCtrl.h"

#include "fs/FileMode.h"
#include "ui/StringUtils.h"

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

void RemoteFileListCtrl::InsertStandardColumns()
{
    InsertColumn(FileColName,        "Name",        wxLIST_FORMAT_LEFT,  240);
    InsertColumn(FileColSize,        "Size",        wxLIST_FORMAT_RIGHT,  90);
    InsertColumn(FileColModified,    "Modified",    wxLIST_FORMAT_LEFT,  130);
    InsertColumn(FileColPermissions, "Permissions", wxLIST_FORMAT_LEFT,  105);
    InsertColumn(FileColOwner,       "Owner",       wxLIST_FORMAT_LEFT,  110);
}

wxString RemoteFileListCtrl::OnGetItemText(long item, long column) const
{
    const term::fs::DirModel* model = provider_ ? provider_() : nullptr;
    if (!model || item < 0) return {};
    const auto row = static_cast<size_t>(item);
    if (row >= model->VisibleCount()) return {};

    const term::transport::FileInfo& e = model->At(row);
    switch (column) {
        case FileColName:
            // Remote names are opaque bytes; a non-UTF-8 name must stay
            // visible rather than silently rendering as empty.
            return DecodeForDisplay(e.isDir ? e.name + "/" : e.name);
        case FileColSize:
            return e.isDir ? wxString("-") : FormatByteSize(e.size);
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
