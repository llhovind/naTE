#include "ui/FileExplorerPane.h"

#include "fs/FileMode.h"
#include "fs/RemotePath.h"
#include "ui/ColorUtils.h"
#include "ui/FilePropertiesDialog.h"
#include "ui/RemoteFileListCtrl.h"
#include "ui/StringUtils.h"

#include <wx/app.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/dnd.h>
#include <wx/textdlg.h>

namespace ui {

namespace {

// wxButton does not inherit its foreground from the parent panel on GTK, so
// every button needs both colours set explicitly or it goes invisible against
// a dark theme.
void StyleButton(wxButton* btn, const wxColour& bg, const wxColour& fg)
{
    btn->SetBackgroundColour(bg);
    btn->SetForegroundColour(fg);
}

// Receives files dragged in from the desktop.
//
// Only inbound drops are handled. Dragging *out* of a virtual wxListCtrl to
// the desktop needs a drag source that can materialise remote files on demand,
// which GTK does not make workable without downloading them first — and
// downloading on hover is not a thing to do behind a user's back.
class PaneFileDropTarget : public wxFileDropTarget {
public:
    using Handler = std::function<void(std::vector<std::string>)>;

    explicit PaneFileDropTarget(Handler handler) : handler_(std::move(handler)) {}

    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& filenames) override
    {
        if (!handler_ || filenames.IsEmpty()) return false;

        std::vector<std::string> paths;
        paths.reserve(filenames.GetCount());
        for (const wxString& name : filenames)
            paths.push_back(std::string(name.ToUTF8()));

        handler_(std::move(paths));
        return true;
    }

private:
    Handler handler_;
};

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FileExplorerPane::FileExplorerPane(wxWindow* parent,
                                   const AppConfig& cfg,
                                   EndpointProvider provider,
                                   PaneEndpoint initial)
    : wxPanel(parent, wxID_ANY)
    , provider_(std::move(provider))
    , cfg_(cfg)
    , guard_([](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); })
{
    auto* outer = new wxBoxSizer(wxVERTICAL);
    BuildToolbar(outer);
    BuildList(outer);

    status_ = new wxStaticText(this, wxID_ANY, "Loading...");
    outer->Add(status_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    SetSizer(outer);
    ApplyConfig(cfg_);

    BindEndpoint(initial);
    RefreshEndpointChoices();
}

void FileExplorerPane::BindEndpoint(const PaneEndpoint& endpoint)
{
    // Everything tied to the previous filesystem goes first: the controller
    // and deleter hold it by reference, and their guards retire any callback
    // still in flight against it.
    controller_.reset();
    deleter_.reset();
    endpoint_ = endpoint;

    if (!endpoint_.Valid()) {
        GoOffline("No filesystem selected.");
        return;
    }

    auto dispatch = [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); };

    controller_ = std::make_unique<term::fs::ExplorerController>(*endpoint_.fs, dispatch);
    controller_->SetListener(this);
    controller_->Model().SetShowHidden(hiddenCheck_->GetValue());
    controller_->Model().SetNameFilter(filterCtrl_->GetValue().ToStdString());

    deleter_ = std::make_unique<term::fs::RemoteDeleter>(*endpoint_.fs, dispatch);

    controller_->NavigateTo(endpoint_.defaultPath.empty() ? "."
                                                          : endpoint_.defaultPath);
    UpdateNavigationState();
    if (onStateChanged_) onStateChanged_();
}

void FileExplorerPane::RefreshEndpointChoices()
{
    if (!endpointChoice_ || !provider_) return;

    choices_ = provider_();

    endpointChoice_->Freeze();
    endpointChoice_->Clear();
    int selected = wxNOT_FOUND;
    for (size_t i = 0; i < choices_.size(); ++i) {
        endpointChoice_->Append(DecodeForDisplay(choices_[i].label));
        // Match on session rather than pointer: a reconnected session keeps
        // its id while its filesystem object may have been replaced.
        if (choices_[i].sessionId == endpoint_.sessionId)
            selected = static_cast<int>(i);
    }
    if (selected != wxNOT_FOUND) endpointChoice_->SetSelection(selected);
    endpointChoice_->Thaw();
}

void FileExplorerPane::OnEndpointSelected(wxCommandEvent&)
{
    const int index = endpointChoice_->GetSelection();
    if (index == wxNOT_FOUND || static_cast<size_t>(index) >= choices_.size()) return;

    const PaneEndpoint& chosen = choices_[static_cast<size_t>(index)];
    if (chosen.sessionId == endpoint_.sessionId && chosen.fs == endpoint_.fs) return;

    BindEndpoint(chosen);
    if (onEndpointChanged_) onEndpointChanged_();
}

void FileExplorerPane::BuildToolbar(wxSizer* outer)
{
    endpointChoice_ = new wxChoice(this, wxID_ANY);
    endpointChoice_->Bind(wxEVT_CHOICE, &FileExplorerPane::OnEndpointSelected, this);
    outer->Add(endpointChoice_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 6);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    backBtn_      = new wxButton(this, wxID_ANY, "<",  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    forwardBtn_   = new wxButton(this, wxID_ANY, ">",  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    upBtn_        = new wxButton(this, wxID_ANY, "Up", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    pathCtrl_     = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    refreshBtn_   = new wxButton(this, wxID_ANY, "Refresh", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    newFolderBtn_ = new wxButton(this, wxID_ANY, "New Folder", wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);

    row->Add(backBtn_,      0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 2);
    row->Add(forwardBtn_,   0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 2);
    row->Add(upBtn_,        0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    row->Add(pathCtrl_,     1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    row->Add(refreshBtn_,   0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 2);
    row->Add(newFolderBtn_, 0, wxALIGN_CENTER_VERTICAL);
    outer->Add(row, 0, wxEXPAND | wxALL, 6);

    auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
    filterLabel_ = new wxStaticText(this, wxID_ANY, "Filter:");
    filterCtrl_  = new wxTextCtrl(this, wxID_ANY);
    filterCtrl_->SetHint("*.conf   or   substring");
    hiddenCheck_ = new wxCheckBox(this, wxID_ANY, "Show hidden");

    filterRow->Add(filterLabel_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    filterRow->Add(filterCtrl_,  1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
    filterRow->Add(hiddenCheck_, 0, wxALIGN_CENTER_VERTICAL);
    outer->Add(filterRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    backBtn_->Bind(wxEVT_BUTTON,    [this](wxCommandEvent&) { if (controller_) controller_->GoBack(); });
    forwardBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (controller_) controller_->GoForward(); });
    upBtn_->Bind(wxEVT_BUTTON,      [this](wxCommandEvent&) { if (controller_) controller_->NavigateUp(); });
    refreshBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (controller_) controller_->Refresh(); });
    newFolderBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { NewFolder(); });
    pathCtrl_->Bind(wxEVT_TEXT_ENTER, &FileExplorerPane::OnGo, this);
    filterCtrl_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!controller_) return;
        controller_->Model().SetNameFilter(filterCtrl_->GetValue().ToStdString());
        RefreshRows();
        UpdateStatus();
    });
    // Alt-arrow history and Ctrl shortcuts are bound here rather than on the
    // list: wxListEvent carries no modifier state, so the plain key events are
    // the only place the combination is visible.
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& evt) {
        if (evt.GetModifiers() == wxMOD_ALT && controller_) {
            if (evt.GetKeyCode() == WXK_LEFT)  { controller_->GoBack();    return; }
            if (evt.GetKeyCode() == WXK_RIGHT) { controller_->GoForward(); return; }
        }
        if (evt.GetModifiers() == wxMOD_CONTROL) {
            switch (evt.GetKeyCode()) {
                case 'L': pathCtrl_->SetFocus();   pathCtrl_->SelectAll();  return;
                case 'F': filterCtrl_->SetFocus(); filterCtrl_->SelectAll(); return;
                case 'H':
                    hiddenCheck_->SetValue(!hiddenCheck_->GetValue());
                    if (controller_) {
                        controller_->Model().SetShowHidden(hiddenCheck_->GetValue());
                        RefreshRows();
                        UpdateStatus();
                    }
                    return;
                default: break;
            }
        }
        evt.Skip();
    });

    hiddenCheck_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        if (!controller_) return;
        controller_->Model().SetShowHidden(hiddenCheck_->GetValue());
        RefreshRows();
        UpdateStatus();
    });
}

void FileExplorerPane::BuildList(wxSizer* outer)
{
    // Multi-select: transfers and deletions routinely act on several rows, and
    // forcing one round trip per file would make the pane tedious to use.
    list_ = new RemoteFileListCtrl(this, [this]() -> const term::fs::DirModel* {
        return controller_ ? &controller_->Model() : nullptr;
    }, 0 /* multi-select: transfers and deletions act on several rows */);
    list_->InsertStandardColumns();
    outer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);

    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,   &FileExplorerPane::OnItemActivated,   this);
    list_->Bind(wxEVT_LIST_COL_CLICK,        &FileExplorerPane::OnColumnClick,     this);
    list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &FileExplorerPane::OnContextMenu,     this);
    list_->Bind(wxEVT_LIST_KEY_DOWN,         &FileExplorerPane::OnListKeyDown,     this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED,    &FileExplorerPane::OnSelectionChanged, this);
    list_->Bind(wxEVT_LIST_ITEM_DESELECTED,  &FileExplorerPane::OnSelectionChanged, this);

    // wx takes ownership of the drop target.
    list_->SetDropTarget(new PaneFileDropTarget(
        [this](std::vector<std::string> paths) {
            if (onLocalFilesDropped_) onLocalFilesDropped_(std::move(paths));
        }));
}

void FileExplorerPane::ApplyConfig(const AppConfig& cfg)
{
    cfg_ = cfg;

    const wxColour bg      = toWx(cfg_.ansiColors[0]);
    const wxColour fg      = toWx(cfg_.ansiColors[7]);
    const wxColour paneBg  = toWx(cfg_.uiColors.frameBackground);

    SetBackgroundColour(paneBg);
    if (list_) {
        list_->SetBackgroundColour(bg);
        list_->SetForegroundColour(fg);
    }
    for (wxTextCtrl* ctrl : {pathCtrl_, filterCtrl_}) {
        if (!ctrl) continue;
        ctrl->SetBackgroundColour(bg);
        ctrl->SetForegroundColour(fg);
    }

    // Loose text sits on the pane background, so its contrast is measured
    // against that rather than borrowed from a UiColors slot defined for a
    // different surface.
    const wxColour labelFg = pickContrasting(paneBg, bg, fg);
    for (wxStaticText* label : {filterLabel_, status_})
        if (label) label->SetForegroundColour(labelFg);
    if (hiddenCheck_) hiddenCheck_->SetForegroundColour(labelFg);
    if (endpointChoice_) {
        endpointChoice_->SetBackgroundColour(bg);
        endpointChoice_->SetForegroundColour(fg);
    }

    const wxColour btnBg = toWx(cfg_.uiColors.tileInactive);
    for (wxButton* btn : {backBtn_, forwardBtn_, upBtn_, refreshBtn_, newFolderBtn_})
        if (btn) StyleButton(btn, btnBg, pickContrasting(btnBg, bg, fg));

    wxPanel::Refresh();
}

// ---------------------------------------------------------------------------
// Controller notifications
// ---------------------------------------------------------------------------

void FileExplorerPane::OnExplorerLoadingChanged(bool loading)
{
    if (loading) SetStatus("Loading...");
    UpdateNavigationState();
}

void FileExplorerPane::OnExplorerContentsChanged()
{
    RefreshRows();

    if (!pendingFocusName_.empty() && controller_) {
        const size_t row = controller_->Model().IndexOfName(pendingFocusName_);
        if (row < controller_->Model().VisibleCount()) {
            const auto item = static_cast<long>(row);
            list_->SetItemState(item, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(item);
        }
        pendingFocusName_.clear();
    }

    UpdateStatus();
    UpdateNavigationState();
    if (onStateChanged_) onStateChanged_();
}

void FileExplorerPane::OnExplorerPathChanged(const std::string& path)
{
    if (pathCtrl_) pathCtrl_->ChangeValue(wxString::FromUTF8(path));
    if (onStateChanged_) onStateChanged_();
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------

void FileExplorerPane::RefreshRows()
{
    if (!list_ || !controller_) return;
    const long count = static_cast<long>(controller_->Model().VisibleCount());
    list_->SetItemCount(count);
    if (count > 0) list_->RefreshItems(0, count - 1);
    else           list_->Refresh();
}

void FileExplorerPane::SetStatus(const wxString& text)
{
    if (status_) status_->SetLabel(text);
}

void FileExplorerPane::UpdateNavigationState()
{
    const bool live    = controller_ != nullptr;
    const bool loading = live && controller_->IsLoading();

    if (backBtn_)      backBtn_->Enable(live && !loading && controller_->CanGoBack());
    if (forwardBtn_)   forwardBtn_->Enable(live && !loading && controller_->CanGoForward());
    if (upBtn_)        upBtn_->Enable(live && !loading && controller_->CurrentPath() != "/");
    if (refreshBtn_)   refreshBtn_->Enable(live && !loading);
    if (newFolderBtn_) newFolderBtn_->Enable(live && !loading);
    if (pathCtrl_)     pathCtrl_->Enable(live);
    if (filterCtrl_)   filterCtrl_->Enable(live);
    if (hiddenCheck_)  hiddenCheck_->Enable(live);
}

void FileExplorerPane::UpdateStatus()
{
    if (!controller_) return;
    const auto& model = controller_->Model();

    wxString text = wxString::Format(
        "%zu director%s, %zu file%s  -  %s",
        model.VisibleDirectoryCount(),
        model.VisibleDirectoryCount() == 1 ? "y" : "ies",
        model.VisibleFileCount(),
        model.VisibleFileCount() == 1 ? "" : "s",
        FormatByteSize(model.VisibleByteTotal()));

    if (model.VisibleCount() != model.TotalCount())
        text += wxString::Format("   (%zu not shown)",
                                 model.TotalCount() - model.VisibleCount());

    if (model.IsPartial())
        text += "   incomplete: " + DecodeForDisplay(model.Error().message);
    else if (model.HasError())
        text = "Error: " + DecodeForDisplay(model.Error().message);

    SetStatus(text);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

const std::string& FileExplorerPane::CurrentPath() const
{
    static const std::string kEmpty;
    return controller_ ? controller_->CurrentPath() : kEmpty;
}

std::vector<FileExplorerPane::Item> FileExplorerPane::SelectedItems() const
{
    std::vector<Item> items;
    if (!controller_ || !list_) return items;

    long index = -1;
    while ((index = list_->GetNextItem(index, wxLIST_NEXT_ALL,
                                       wxLIST_STATE_SELECTED)) != wxNOT_FOUND) {
        const auto row = static_cast<size_t>(index);
        if (row >= controller_->Model().VisibleCount()) continue;
        const term::transport::FileInfo& e = controller_->Model().At(row);
        items.push_back({controller_->PathOf(row), e.name, e.isDir, e.isSymlink, e.size});
    }
    return items;
}

void FileExplorerPane::Refresh()
{
    if (controller_) controller_->Refresh();
}

void FileExplorerPane::RefreshFocusing(std::string focusName)
{
    pendingFocusName_ = std::move(focusName);
    Refresh();
}

void FileExplorerPane::GoOffline(const wxString& reason)
{
    controller_.reset();
    deleter_.reset();
    endpoint_.fs = nullptr;
    if (list_) { list_->SetItemCount(0); list_->Refresh(); }
    SetStatus(reason);
    UpdateNavigationState();
    if (onStateChanged_) onStateChanged_();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void FileExplorerPane::OnGo(wxCommandEvent&)
{
    if (!controller_) return;
    controller_->NavigateTo(pathCtrl_->GetValue().Trim().ToStdString());
}

void FileExplorerPane::OnSelectionChanged(wxListEvent& evt)
{
    if (onStateChanged_) onStateChanged_();
    evt.Skip();
}

void FileExplorerPane::OnItemActivated(wxListEvent& evt)
{
    if (!controller_) return;
    const auto row = static_cast<size_t>(evt.GetIndex());

    controller_->Activate(row,
        [this](term::fs::ActivationResult result, std::string path,
               term::transport::FsError err) {
            switch (result) {
                case term::fs::ActivationResult::Navigated:
                    break;
                case term::fs::ActivationResult::IsFile:
                    if (onOpenInEditor_)
                        onOpenInEditor_(endpoint_.sessionId, std::move(path));
                    break;
                case term::fs::ActivationResult::Failed:
                    SetStatus(wxString::Format("Cannot open '%s': %s",
                                               DecodeForDisplay(path),
                                               DecodeForDisplay(err.message)));
                    break;
            }
        });
}

void FileExplorerPane::OnColumnClick(wxListEvent& evt)
{
    if (!controller_) return;
    using term::fs::SortKey;
    using term::fs::SortOrder;

    static constexpr SortKey kColumnKeys[FileColumnCount] = {
        SortKey::Name, SortKey::Size, SortKey::Modified,
        SortKey::Permissions, SortKey::Owner,
    };

    const int col = evt.GetColumn();
    if (col < 0 || col >= FileColumnCount) return;

    auto& model = controller_->Model();
    const SortKey key = kColumnKeys[col];
    const SortOrder order =
        (model.Sort() == key && model.Order() == SortOrder::Ascending)
            ? SortOrder::Descending
            : SortOrder::Ascending;

    model.SetSort(key, order);
    RefreshRows();
}

void FileExplorerPane::FocusList()
{
    if (list_) list_->SetFocus();
}

void FileExplorerPane::OnListKeyDown(wxListEvent& evt)
{
    const size_t row = static_cast<size_t>(evt.GetIndex());
    switch (evt.GetKeyCode()) {
        case WXK_DELETE:    DeleteSelection(); break;
        case WXK_F2:        RenameRow(row);    break;
        case WXK_F5:        if (controller_) controller_->Refresh(); break;
        // Backspace goes up a level, as in every file manager.
        case WXK_BACK:      if (controller_) controller_->NavigateUp(); break;
        default:            evt.Skip(); break;
    }
}

void FileExplorerPane::OnContextMenu(wxListEvent& evt)
{
    if (!controller_) return;
    const auto row = static_cast<size_t>(evt.GetIndex());
    if (row >= controller_->Model().VisibleCount()) return;

    const bool isDir = controller_->Model().At(row).isDir;
    const size_t selectionCount = SelectedItems().size();

    wxMenu menu;
    auto* editItem   = menu.Append(wxID_ANY, "Open in Editor");
    auto* copyItem   = menu.Append(wxID_ANY, "Copy Path");
    menu.AppendSeparator();
    auto* newItem    = menu.Append(wxID_ANY, "New Folder...");
    auto* renameItem = menu.Append(wxID_ANY, "Rename...\tF2");
    auto* deleteItem = menu.Append(
        wxID_ANY, selectionCount > 1
                      ? wxString::Format("Delete %zu Items...\tDel", selectionCount)
                      : wxString("Delete...\tDel"));
    menu.AppendSeparator();
    auto* propsItem  = menu.Append(wxID_ANY, "Properties...");

    editItem->Enable(!isDir && onOpenInEditor_ != nullptr);
    // Renaming is inherently single-target; the item says so by being disabled
    // rather than silently acting on one of several selected rows.
    renameItem->Enable(selectionCount <= 1);

    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { EditRow(row); }, editItem->GetId());
    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { CopyPathOf(row); }, copyItem->GetId());
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { NewFolder(); }, newItem->GetId());
    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { RenameRow(row); }, renameItem->GetId());
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { DeleteSelection(); }, deleteItem->GetId());
    menu.Bind(wxEVT_MENU, [this, row](wxCommandEvent&) { ShowPropertiesFor(row); },
              propsItem->GetId());

    PopupMenu(&menu);
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

bool FileExplorerPane::RequireLive()
{
    if (controller_) return true;
    wxMessageBox("This pane is no longer connected.",
                 "File Explorer", wxOK | wxICON_INFORMATION, this);
    return false;
}

void FileExplorerPane::AfterWrite(const wxString& what,
                                  const term::transport::FsError& err,
                                  std::string focusName)
{
    if (err.Failed()) {
        wxMessageBox(what + " failed:\n\n" + DecodeForDisplay(err.message),
                     "File Explorer", wxOK | wxICON_ERROR, this);
        return;
    }
    RefreshFocusing(std::move(focusName));
}

void FileExplorerPane::EditRow(size_t row)
{
    if (!controller_ || !onOpenInEditor_) return;
    if (row >= controller_->Model().VisibleCount()) return;
    if (controller_->Model().At(row).isDir) return;
    onOpenInEditor_(endpoint_.sessionId, controller_->PathOf(row));
}

void FileExplorerPane::CopyPathOf(size_t row)
{
    if (!controller_ || row >= controller_->Model().VisibleCount()) return;

    const std::string path = controller_->PathOf(row);
    if (!wxTheClipboard->Open()) return;
    wxTheClipboard->SetData(new wxTextDataObject(DecodeForDisplay(path)));
    wxTheClipboard->Close();
    SetStatus(wxString::Format("Copied %s", DecodeForDisplay(path)));
}

void FileExplorerPane::ShowPropertiesFor(size_t row)
{
    if (!controller_ || row >= controller_->Model().VisibleCount()) return;

    const std::string path = controller_->PathOf(row);
    const std::string name = controller_->Model().At(row).name;

    FilePropertiesDialog dlg(this, cfg_, controller_->Model().At(row), path,
                             *endpoint_.fs);
    if (dlg.ShowModal() != wxID_OK || !dlg.PermissionsChanged()) return;

    auto ctx = guard_.For(this);
    endpoint_.fs->SetPermissions(path, dlg.SelectedMode(),
        [ctx, name](term::transport::FsError err) {
            ctx.Post([name, err = std::move(err)](FileExplorerPane& p) mutable {
                p.AfterWrite("Change permissions", err, name);
            });
        });
}

void FileExplorerPane::NewFolder()
{
    if (!RequireLive()) return;

    wxTextEntryDialog dlg(this, "Name for the new folder:", "New Folder");
    if (dlg.ShowModal() != wxID_OK) return;

    const std::string name = dlg.GetValue().Trim().Trim(false).ToStdString();
    if (name.empty()) return;
    if (name.find('/') != std::string::npos) {
        wxMessageBox("A folder name cannot contain '/'.",
                     "New Folder", wxOK | wxICON_WARNING, this);
        return;
    }

    const std::string path = term::fs::path::Join(controller_->CurrentPath(), name);
    auto ctx = guard_.For(this);
    endpoint_.fs->MakeDirectory(path, 0755,
        [ctx, name](term::transport::FsError err) {
            ctx.Post([name, err = std::move(err)](FileExplorerPane& p) mutable {
                p.AfterWrite("Create folder", err, name);
            });
        });
}

void FileExplorerPane::RenameRow(size_t row)
{
    if (!RequireLive()) return;
    if (row >= controller_->Model().VisibleCount()) return;

    const std::string oldName = controller_->Model().At(row).name;
    const std::string oldPath = controller_->PathOf(row);

    wxTextEntryDialog dlg(this, "New name:", "Rename", DecodeForDisplay(oldName));
    if (dlg.ShowModal() != wxID_OK) return;

    const std::string newName = dlg.GetValue().Trim().Trim(false).ToStdString();
    if (newName.empty() || newName == oldName) return;
    if (newName.find('/') != std::string::npos) {
        wxMessageBox("A name cannot contain '/'.",
                     "Rename", wxOK | wxICON_WARNING, this);
        return;
    }

    const std::string newPath =
        term::fs::path::Join(controller_->CurrentPath(), newName);

    // Check the destination first rather than trusting the filesystem to
    // refuse. POSIX rename() replaces silently, and the SFTP servers that do
    // refuse are being polite rather than obeying the protocol.
    auto ctx = guard_.For(this);
    endpoint_.fs->Stat(newPath,
        [ctx, oldPath, newPath, newName](term::transport::FileInfo,
                                         term::transport::FsError statErr) {
            ctx.Post([oldPath, newPath, newName,
                      statErr = std::move(statErr)](FileExplorerPane& p) mutable {
                if (statErr.code != term::transport::FsErrorCode::NoSuchFile) {
                    wxMessageBox(
                        wxString::Format("'%s' already exists in this directory.",
                                         DecodeForDisplay(newName)),
                        "Rename", wxOK | wxICON_WARNING, &p);
                    return;
                }
                auto inner = p.guard_.For(&p);
                p.endpoint_.fs->Rename(oldPath, newPath,
                    [inner, newName](term::transport::FsError err) {
                        inner.Post([newName, err = std::move(err)](
                                       FileExplorerPane& q) mutable {
                            q.AfterWrite("Rename", err, newName);
                        });
                    });
            });
        });
}

bool FileExplorerPane::ConfirmDeletion(const term::fs::DeletePlan& plan,
                                       const wxString& target)
{
    if (plan.Empty()) return false;

    wxString detail;
    if (plan.dirCount + plan.fileCount > 1) {
        detail = wxString::Format(
            "This will permanently delete %zu file%s and %zu director%s (%s).",
            plan.fileCount, plan.fileCount == 1 ? "" : "s",
            plan.dirCount,  plan.dirCount == 1 ? "y" : "ies",
            FormatByteSize(plan.totalBytes));
    } else {
        detail = "This cannot be undone.";
    }

    if (plan.error.Failed()) {
        detail += "\n\nPart of this could not be read (" +
                  DecodeForDisplay(plan.error.message) +
                  "), so more may exist than is listed above, and the deletion "
                  "may stop partway.";
    }

    wxMessageDialog dlg(this,
                        wxString::Format("Delete %s?", target),
                        "Delete",
                        wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    dlg.SetExtendedMessage(detail);
    dlg.SetYesNoLabels("Delete", "Cancel");
    return dlg.ShowModal() == wxID_YES;
}

void FileExplorerPane::DeleteSelection()
{
    if (!RequireLive() || !deleter_) return;

    const std::vector<Item> items = SelectedItems();
    if (items.empty()) return;

    std::vector<term::fs::DeleteTarget> targets;
    targets.reserve(items.size());
    for (const Item& item : items)
        targets.push_back({item.path, item.isDir, item.isSymlink, item.size});

    const wxString label =
        items.size() == 1 ? wxString::Format("'%s'", DecodeForDisplay(items.front().name))
                          : wxString::Format("%zu items", items.size());

    SetStatus("Examining " + label + "...");

    auto ctx = guard_.For(this);
    deleter_->Plan(std::move(targets),
        [ctx, label](term::fs::DeletePlan plan) {
            ctx.Post([label, plan = std::move(plan)](FileExplorerPane& p) mutable {
                p.UpdateStatus();
                if (!p.ConfirmDeletion(plan, label)) return;
                if (!p.deleter_) return;

                auto inner = p.guard_.For(&p);
                p.deleter_->Execute(std::move(plan),
                    [inner](size_t done, size_t total) {
                        inner.Post([done, total](FileExplorerPane& q) {
                            q.SetStatus(wxString::Format("Deleting... %zu/%zu",
                                                         done, total));
                        });
                    },
                    [inner](term::transport::FsError err) {
                        inner.Post([err = std::move(err)](FileExplorerPane& q) mutable {
                            q.AfterWrite("Delete", err, {});
                        });
                    });
            });
        });
}

} // namespace ui
