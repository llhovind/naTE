#pragma once

#include "config/Config.h"
#include "fs/DirModel.h"

#include <array>
#include <functional>

// wxItemAttr arrives with this, and only with this: wx/itemattr.h declares the
// class without including what its members are made of, so including it first
// does not compile.
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

// The config layer sizes and persists these columns without being able to name
// the enum, so the count it assumes is checked here instead of drifting.
static_assert(FileColumnCount == kFileExplorerColumnCount,
              "kFileExplorerColumnCount must match the FileColumn enum");

// Virtual list over a DirModel.
//
// Rows are rendered on demand from whatever the provider returns, so the
// control holds no copy of the listing and cannot fall out of step with the
// model. Virtual rather than populated because a directory can hold tens of
// thousands of entries and an insert per row makes such a listing unusable.
class RemoteFileListCtrl : public wxListCtrl {
public:
    using ModelProvider = std::function<const term::fs::DirModel*()>;
    using ColumnWidths  = std::array<int, kFileExplorerColumnCount>;

    RemoteFileListCtrl(wxWindow* parent, ModelProvider provider, long style);

    // Adds the standard columns at the given widths. Callers that want a
    // narrower view can skip this and insert their own. The default argument
    // serves the views that do not persist widths, so they need not know that
    // anything else does.
    void InsertStandardColumns(
        const ColumnWidths& widths = kDefaultFileExplorerColumnWidths);

    // The widths as they stand now, for an owner that saves them. Read on
    // demand rather than tracked: wx is the one that knows how wide a column
    // ended up after a drag, so asking it is the only answer that cannot go
    // stale.
    ColumnWidths CurrentColumnWidths() const;

    // Resizes the standard columns in place. Setting a width does not raise a
    // drag event, so this cannot feed back into whatever asked for it.
    void ApplyColumnWidths(const ColumnWidths& widths);

    // Colour for a link that leads nowhere. Until this is set — and it is the
    // owner's to set, because the readable red depends on the palette the
    // listing is drawn in — dangling links are drawn like any other row.
    void SetBrokenLinkColour(const wxColour& colour);

protected:
    wxString OnGetItemText(long item, long column) const override;
    // A dangling link is the one row whose state a user cannot see any other
    // way: the name, size and mode all still read as an ordinary link.
    wxItemAttr* OnGetItemAttr(long item) const override;

private:
    ModelProvider provider_;
    // Held rather than built per row: wx keeps the pointer after OnGetItemAttr
    // returns. Mutable because that query is const and wx wants to be handed a
    // pointer it could write through, not because anything here writes to it.
    mutable wxItemAttr brokenAttr_;
    bool               hasBrokenColour_ = false;
};

} // namespace ui
