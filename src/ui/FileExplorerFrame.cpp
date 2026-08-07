#include "ui/FileExplorerFrame.h"

#include "fs/FileMode.h"
#include "fs/RemotePath.h"
#include "ui/ColorUtils.h"
#include "ui/FilePropertiesDialog.h"
#include "ui/StringUtils.h"

#include <cmath>
#include <ctime>

#include <wx/app.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace ui {

namespace {

// Column order in the listing.
enum Column { ColName = 0, ColSize, ColModified, ColPermissions, ColOwner, ColCount };

constexpr int kInitialWidth  = 820;
constexpr int kInitialHeight = 560;
constexpr int kMinWidth      = 480;
constexpr int kMinHeight     = 320;

// Formats a byte count the way a person reads it. Binary units, because these
// are file sizes on a POSIX filesystem and that is what `ls -h` reports.
wxString FormatSize(uint64_t bytes)
{
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes);
    size_t unit  = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) return wxString::Format("%llu B",
                                           static_cast<unsigned long long>(bytes));
    return wxString::Format("%.1f %s", value, units[unit]);
}

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

// Picks whichever of the theme's text colours actually reads against bg.
//
// Derived here rather than taken from UiColors because every slot there is
// defined against a *specific* background (tabText against tileInactive, and
// so on); none of them is "text on the window background". Measuring the
// luminance of the real background is the only way to be right on both light
// and dark palettes.
wxColour ContrastingText(const wxColour& bg, const AppConfig& cfg)
{
    const double luminance = 0.299 * bg.Red() + 0.587 * bg.Green() + 0.114 * bg.Blue();
    const wxColour a = toWx(cfg.ansiColors[0]);
    const wxColour b = toWx(cfg.ansiColors[7]);
    const double lumA = 0.299 * a.Red() + 0.587 * a.Green() + 0.114 * a.Blue();
    // Whichever of the two sits further from the background in luminance.
    const bool preferA = std::abs(lumA - luminance) >
                         std::abs((0.299 * b.Red() + 0.587 * b.Green() +
                                   0.114 * b.Blue()) - luminance);
    return preferA ? a : b;
}

// wxButton does not inherit its foreground from the parent panel on GTK, so
// every button needs both colours set explicitly or it goes invisible against
// a dark theme.
void StyleButton(wxButton* btn, const wxColour& bg, const wxColour& fg)
{
    btn->SetBackgroundColour(bg);
    btn->SetForegroundColour(fg);
}

} // namespace

// ---------------------------------------------------------------------------
// RemoteFileListCtrl
// ---------------------------------------------------------------------------

// Rows are rendered on demand from whatever the provider returns, so the
// control holds no copy of the listing to fall out of step with the model.
class RemoteFileListCtrl : public wxListCtrl {
public:
    using ModelProvider = std::function<const term::fs::DirModel*()>;

    RemoteFileListCtrl(wxWindow* parent, ModelProvider provider)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL)
        , provider_(std::move(provider))
    {}

protected:
    wxString OnGetItemText(long item, long column) const override
    {
        const term::fs::DirModel* model = provider_ ? provider_() : nullptr;
        if (!model || item < 0) return {};
        const auto row = static_cast<size_t>(item);
        if (row >= model->VisibleCount()) return {};

        const term::transport::FileInfo& e = model->At(row);
        switch (column) {
            case ColName:
                // Remote names are opaque bytes; a non-UTF-8 name must stay
                // visible rather than silently rendering as empty.
                return DecodeForDisplay(e.isDir ? e.name + "/" : e.name);
            case ColSize:
                return e.isDir ? wxString("-") : FormatSize(e.size);
            case ColModified:
                return FormatTime(e.mtime);
            case ColPermissions:
                return wxString::FromUTF8(term::fs::FormatPermissions(e.mode));
            case ColOwner:
                return DecodeForDisplay(e.group.empty() ? e.owner
                                                        : e.owner + ":" + e.group);
            default:
                return {};
        }
    }

private:
    ModelProvider provider_;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FileExplorerFrame::FileExplorerFrame(wxWindow* parent,
                                     term::session::SessionId sessionId,
                                     term::session::SessionManager& sm,
                                     const AppConfig& cfg,
                                     std::string remoteDescription,
                                     std::function<void(std::string)> onOpenInEditor)
    : wxFrame(parent, wxID_ANY,
              remoteDescription.empty()
                  ? wxString("File Explorer")
                  : wxString::Format("Files - %s",
                                     wxString::FromUTF8(remoteDescription)),
              wxDefaultPosition, wxSize(kInitialWidth, kInitialHeight))
    , sessionId_(sessionId)
    , sm_(sm)
    , cfg_(cfg)
    , onOpenInEditor_(std::move(onOpenInEditor))
{
    auto* outer = new wxBoxSizer(wxVERTICAL);
    BuildToolbar(this, outer);
    BuildList(outer);
    SetSizer(outer);
    SetMinSize(wxSize(kMinWidth, kMinHeight));

    status_ = CreateStatusBar();

    // wxEVT_DESTROY rather than wxEVT_CLOSE_WINDOW: closing the parent window
    // destroys this frame directly, without ever sending a close event, and
    // the owner would be left holding a dangling pointer.
    Bind(wxEVT_DESTROY, &FileExplorerFrame::OnDestroy, this);

    ApplyConfig(cfg_);

    term::transport::IRemoteFileSystem* remote = sm_.GetRemoteFileSystem(sessionId_);
    if (!remote) {
        // Should not happen — the menu entry is gated on the capability — but
        // a window that says why it is empty beats one that silently is.
        status_->SetStatusText("This session has no remote filesystem.");
        UpdateNavigationState();
        return;
    }

    controller_ = std::make_unique<term::fs::ExplorerController>(
        *remote,
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); });
    controller_->SetListener(this);
    controller_->Model().SetShowHidden(hiddenCheck_->GetValue());

    // Start where the shell is, when we know it. The controller canonicalises
    // "." server-side, so an unknown working directory still lands somewhere
    // sensible rather than at the root.
    const std::string cwd = sm_.GetCurrentWorkingDir(sessionId_);
    controller_->NavigateTo(cwd.empty() ? "." : cwd);
}

void FileExplorerFrame::BuildToolbar(wxWindow* parent, wxSizer* outer)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);

    backBtn_    = new wxButton(parent, wxID_ANY, "<",  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    forwardBtn_ = new wxButton(parent, wxID_ANY, ">",  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    upBtn_      = new wxButton(parent, wxID_ANY, "Up", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pathCtrl_   = new wxTextCtrl(parent, wxID_ANY, wxEmptyString,
                                 wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    refreshBtn_ = new wxButton(parent, wxID_ANY, "Refresh", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);

    row->Add(backBtn_,    0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 2);
    row->Add(forwardBtn_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 2);
    row->Add(upBtn_,      0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    row->Add(pathCtrl_,   1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    row->Add(refreshBtn_, 0, wxALIGN_CENTER_VERTICAL);
    outer->Add(row, 0, wxEXPAND | wxALL, 8);

    auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
    filterLabel_ = new wxStaticText(parent, wxID_ANY, "Filter:");
    filterCtrl_ = new wxTextCtrl(parent, wxID_ANY);
    filterCtrl_->SetHint("*.conf   or   substring");
    hiddenCheck_ = new wxCheckBox(parent, wxID_ANY, "Show hidden");

    filterRow->Add(filterLabel_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    filterRow->Add(filterCtrl_,  1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
    filterRow->Add(hiddenCheck_, 0, wxALIGN_CENTER_VERTICAL);
    outer->Add(filterRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    backBtn_->Bind(wxEVT_BUTTON,    &FileExplorerFrame::OnBack,    this);
    forwardBtn_->Bind(wxEVT_BUTTON, &FileExplorerFrame::OnForward, this);
    upBtn_->Bind(wxEVT_BUTTON,      &FileExplorerFrame::OnUp,      this);
    refreshBtn_->Bind(wxEVT_BUTTON, &FileExplorerFrame::OnRefresh, this);
    pathCtrl_->Bind(wxEVT_TEXT_ENTER, &FileExplorerFrame::OnGo,    this);
    filterCtrl_->Bind(wxEVT_TEXT,   &FileExplorerFrame::OnFilterChanged, this);
    hiddenCheck_->Bind(wxEVT_CHECKBOX, &FileExplorerFrame::OnShowHiddenToggled, this);
}

void FileExplorerFrame::BuildList(wxSizer* outer)
{
    list_ = new RemoteFileListCtrl(this, [this]() -> const term::fs::DirModel* {
        return controller_ ? &controller_->Model() : nullptr;
    });
    list_->InsertColumn(ColName,        "Name",        wxLIST_FORMAT_LEFT,  280);
    list_->InsertColumn(ColSize,        "Size",        wxLIST_FORMAT_RIGHT,  90);
    list_->InsertColumn(ColModified,    "Modified",    wxLIST_FORMAT_LEFT,  140);
    list_->InsertColumn(ColPermissions, "Permissions", wxLIST_FORMAT_LEFT,  110);
    list_->InsertColumn(ColOwner,       "Owner",       wxLIST_FORMAT_LEFT,  120);
    outer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,   &FileExplorerFrame::OnItemActivated, this);
    list_->Bind(wxEVT_LIST_COL_CLICK,        &FileExplorerFrame::OnColumnClick,   this);
    list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &FileExplorerFrame::OnContextMenu,   this);
}

void FileExplorerFrame::ApplyConfig(const AppConfig& cfg)
{
    cfg_ = cfg;

    // Terminal colours rather than tile chrome: this window is mostly a wall
    // of monospaced text, so it should read like the terminal it belongs to.
    const wxColour bg = toWx(cfg_.ansiColors[0]);
    const wxColour fg = toWx(cfg_.ansiColors[7]);
    const wxColour frameBg = toWx(cfg_.uiColors.frameBackground);

    SetBackgroundColour(frameBg);
    if (list_) {
        list_->SetBackgroundColour(bg);
        list_->SetForegroundColour(fg);
    }
    for (wxTextCtrl* ctrl : {pathCtrl_, filterCtrl_}) {
        if (!ctrl) continue;
        ctrl->SetBackgroundColour(bg);
        ctrl->SetForegroundColour(fg);
    }

    // Loose labels sit on the frame background, so their contrast has to be
    // computed against *that*. uiColors.tabText would be the tempting choice
    // and is the wrong one: it is defined to contrast against tileInactive,
    // and on a light palette it resolves to a near-white that vanishes here.
    const wxColour labelFg = ContrastingText(frameBg, cfg_);
    if (filterLabel_) filterLabel_->SetForegroundColour(labelFg);
    if (hiddenCheck_) hiddenCheck_->SetForegroundColour(labelFg);

    // Buttons keep the chrome pairing, where tabText's contract does hold.
    for (wxButton* btn : {backBtn_, forwardBtn_, upBtn_, refreshBtn_})
        if (btn) StyleButton(btn, toWx(cfg_.uiColors.tileInactive),
                             toWx(cfg_.uiColors.tabText));

    Refresh();
}

// ---------------------------------------------------------------------------
// Controller notifications
// ---------------------------------------------------------------------------

void FileExplorerFrame::OnExplorerLoadingChanged(bool loading)
{
    if (loading && status_) status_->SetStatusText("Loading...");
    UpdateNavigationState();
}

void FileExplorerFrame::OnExplorerContentsChanged()
{
    RefreshRows();
    UpdateStatus();
    UpdateNavigationState();
}

void FileExplorerFrame::OnExplorerPathChanged(const std::string& path)
{
    if (pathCtrl_) pathCtrl_->ChangeValue(wxString::FromUTF8(path));
}

void FileExplorerFrame::RefreshRows()
{
    if (!list_ || !controller_) return;
    const long count = static_cast<long>(controller_->Model().VisibleCount());
    list_->SetItemCount(count);
    // Virtual lists cache rendered rows; without this the previous
    // directory's text can survive a navigation.
    if (count > 0) list_->RefreshItems(0, count - 1);
    else           list_->Refresh();
}

void FileExplorerFrame::UpdateNavigationState()
{
    const bool live    = controller_ != nullptr;
    const bool loading = live && controller_->IsLoading();

    if (backBtn_)    backBtn_->Enable(live && !loading && controller_->CanGoBack());
    if (forwardBtn_) forwardBtn_->Enable(live && !loading && controller_->CanGoForward());
    if (upBtn_)      upBtn_->Enable(live && !loading && controller_->CurrentPath() != "/");
    if (refreshBtn_) refreshBtn_->Enable(live && !loading);
    if (pathCtrl_)   pathCtrl_->Enable(live);
    if (filterCtrl_) filterCtrl_->Enable(live);
    if (hiddenCheck_) hiddenCheck_->Enable(live);
}

void FileExplorerFrame::UpdateStatus()
{
    if (!status_ || !controller_) return;
    const auto& model = controller_->Model();

    wxString text = wxString::Format(
        "%zu director%s, %zu file%s  -  %s",
        model.VisibleDirectoryCount(),
        model.VisibleDirectoryCount() == 1 ? "y" : "ies",
        model.VisibleFileCount(),
        model.VisibleFileCount() == 1 ? "" : "s",
        FormatSize(model.VisibleByteTotal()));

    // Covers both exclusion rules — the name filter and the hidden-file
    // toggle — so it must not claim a single cause for the difference.
    if (model.VisibleCount() != model.TotalCount())
        text += wxString::Format("   (%zu not shown)",
                                 model.TotalCount() - model.VisibleCount());

    // A listing that failed partway still shows its rows, so the status line
    // is where the incompleteness has to be admitted.
    if (model.IsPartial())
        text += "   incomplete: " + DecodeForDisplay(model.Error().message);
    else if (model.HasError())
        text = "Error: " + DecodeForDisplay(model.Error().message);

    status_->SetStatusText(text);
}

// ---------------------------------------------------------------------------
// Navigation events
// ---------------------------------------------------------------------------

void FileExplorerFrame::OnGo(wxCommandEvent&)
{
    if (!controller_) return;
    controller_->NavigateTo(pathCtrl_->GetValue().Trim().ToStdString());
}

void FileExplorerFrame::OnUp(wxCommandEvent&)      { if (controller_) controller_->NavigateUp(); }
void FileExplorerFrame::OnBack(wxCommandEvent&)    { if (controller_) controller_->GoBack(); }
void FileExplorerFrame::OnForward(wxCommandEvent&) { if (controller_) controller_->GoForward(); }
void FileExplorerFrame::OnRefresh(wxCommandEvent&) { if (controller_) controller_->Refresh(); }

void FileExplorerFrame::OnItemActivated(wxListEvent& evt)
{
    if (!controller_) return;
    const auto row = static_cast<size_t>(evt.GetIndex());

    controller_->Activate(row,
        [this](term::fs::ActivationResult result, std::string path,
               term::transport::FsError err) {
            switch (result) {
                case term::fs::ActivationResult::Navigated:
                    break;   // the controller already moved us
                case term::fs::ActivationResult::IsFile:
                    if (onOpenInEditor_) onOpenInEditor_(std::move(path));
                    break;
                case term::fs::ActivationResult::Failed:
                    ReportError(wxString::Format("Cannot open '%s'",
                                                 DecodeForDisplay(path)), err);
                    break;
            }
        });
}

void FileExplorerFrame::OnColumnClick(wxListEvent& evt)
{
    if (!controller_) return;
    using term::fs::SortKey;
    using term::fs::SortOrder;

    static constexpr SortKey kColumnKeys[ColCount] = {
        SortKey::Name, SortKey::Size, SortKey::Modified,
        SortKey::Permissions, SortKey::Owner,
    };

    const int col = evt.GetColumn();
    if (col < 0 || col >= ColCount) return;

    auto& model = controller_->Model();
    const SortKey key = kColumnKeys[col];
    // Clicking the active column flips direction; clicking a new one starts
    // ascending, which is the least surprising behaviour in every file manager.
    const SortOrder order =
        (model.Sort() == key && model.Order() == SortOrder::Ascending)
            ? SortOrder::Descending
            : SortOrder::Ascending;

    model.SetSort(key, order);
    RefreshRows();
}

void FileExplorerFrame::OnFilterChanged(wxCommandEvent&)
{
    if (!controller_) return;
    controller_->Model().SetNameFilter(filterCtrl_->GetValue().ToStdString());
    RefreshRows();
    UpdateStatus();
}

void FileExplorerFrame::OnShowHiddenToggled(wxCommandEvent&)
{
    if (!controller_) return;
    controller_->Model().SetShowHidden(hiddenCheck_->GetValue());
    RefreshRows();
    UpdateStatus();
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void FileExplorerFrame::OnContextMenu(wxListEvent& evt)
{
    if (!controller_) return;
    const auto row = static_cast<size_t>(evt.GetIndex());
    if (row >= controller_->Model().VisibleCount()) return;

    const bool isDir = controller_->Model().At(row).isDir;

    wxMenu menu;
    auto* editItem  = menu.Append(wxID_ANY, "Open in Editor");
    auto* copyItem  = menu.Append(wxID_ANY, "Copy Path");
    menu.AppendSeparator();
    auto* propsItem = menu.Append(wxID_ANY, "Properties...");

    // Editing a directory is meaningless; the item stays visible but disabled
    // so the menu's shape does not shift between rows.
    editItem->Enable(!isDir);

    // Every action targets the row that was right-clicked, not the selected
    // one: wx does not select a row on right-click, so acting on the selection
    // would quietly operate on a different file than the one under the cursor.
    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { EditRow(row); }, editItem->GetId());
    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { CopyPathOf(row); }, copyItem->GetId());
    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { ShowPropertiesFor(row); },
              propsItem->GetId());

    PopupMenu(&menu);
}

void FileExplorerFrame::EditRow(size_t row)
{
    if (!controller_ || !onOpenInEditor_) return;
    if (row >= controller_->Model().VisibleCount()) return;
    if (controller_->Model().At(row).isDir) return;
    onOpenInEditor_(controller_->PathOf(row));
}

void FileExplorerFrame::CopyPathOf(size_t row)
{
    if (!controller_ || row >= controller_->Model().VisibleCount()) return;

    const std::string path = controller_->PathOf(row);
    if (!wxTheClipboard->Open()) return;
    wxTheClipboard->SetData(new wxTextDataObject(DecodeForDisplay(path)));
    wxTheClipboard->Close();
    if (status_)
        status_->SetStatusText(wxString::Format("Copied %s",
                                                DecodeForDisplay(path)));
}

void FileExplorerFrame::ShowPropertiesFor(size_t row)
{
    if (!controller_ || row >= controller_->Model().VisibleCount()) return;

    term::transport::IRemoteFileSystem* remote = sm_.GetRemoteFileSystem(sessionId_);
    if (!remote) return;

    FilePropertiesDialog dlg(this, cfg_, controller_->Model().At(row),
                             controller_->PathOf(row), *remote);
    dlg.ShowModal();
}

void FileExplorerFrame::ReportError(const wxString& what,
                                    const term::transport::FsError& err)
{
    if (status_)
        status_->SetStatusText(what + ": " + DecodeForDisplay(err.message));
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

void FileExplorerFrame::OnSessionEnded()
{
    // Dropping the controller retires any in-flight callback and makes every
    // operation a no-op. The window stays so it does not disappear from under
    // the user's cursor mid-click.
    controller_.reset();
    if (list_) { list_->SetItemCount(0); list_->Refresh(); }
    if (status_) status_->SetStatusText("Session closed.");
    UpdateNavigationState();
    SetTitle(GetTitle() + " (closed)");
}

void FileExplorerFrame::OnDestroy(wxWindowDestroyEvent& evt)
{
    // Destroy events from this frame's own children propagate up to here, so
    // only the frame's own destruction should notify the owner.
    if (evt.GetWindow() != this) { evt.Skip(); return; }

    if (onClosed_) {
        auto cb = std::move(onClosed_);
        onClosed_ = nullptr;
        cb();
    }
    evt.Skip();
}

} // namespace ui
