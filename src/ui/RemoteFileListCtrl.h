#pragma once

#include "fs/DirModel.h"

#include <functional>

#include <wx/listctrl.h>

namespace ui {

// Column order shared by every listing view.
enum FileColumn {
    FileColName = 0,
    FileColSize,
    FileColModified,
    FileColPermissions,
    FileColOwner,
    FileColumnCount,
};

// Virtual list over a DirModel.
//
// Rows are rendered on demand from whatever the provider returns, so the
// control holds no copy of the listing and cannot fall out of step with the
// model. Virtual rather than populated because a directory can hold tens of
// thousands of entries and an insert per row makes such a listing unusable.
class RemoteFileListCtrl : public wxListCtrl {
public:
    using ModelProvider = std::function<const term::fs::DirModel*()>;

    RemoteFileListCtrl(wxWindow* parent, ModelProvider provider, long style);

    // Adds the standard columns. Callers that want a narrower view can skip
    // this and insert their own.
    void InsertStandardColumns();

protected:
    wxString OnGetItemText(long item, long column) const override;

private:
    ModelProvider provider_;
};

} // namespace ui
