#include "ui/FileExplorerPane.h"

#include "fs/FileMode.h"
#include "fs/RemotePath.h"
#include "ui/ColorUtils.h"
#include "ui/FilePropertiesDialog.h"
#include "ui/RemoteFileListCtrl.h"
#include "ui/StatusTone.h"
#include "ui/StringUtils.h"
#include "ui/ToolButton.h"

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

// wx has no positive flag for multi-selection: it is the default, and
// wxLC_SINGLE_SEL is the opt-out. Named so the style argument is not a bare 0.
constexpr long kMultiSelect = 0;

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

    // The spinner leads the line rather than trailing it: it is the thing that
    // has to be seen first, and at the end of a status line that can run the
    // width of the pane it would sit wherever the sentence happened to stop.
    status_  = new wxStaticText(this, wxID_ANY, "Loading...");
    spinner_ = MakeStatusSpinner(this, status_);

    auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
    statusRow->Add(spinner_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    statusRow->Add(status_,  1, wxALIGN_CENTER_VERTICAL);
    outer->Add(statusRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

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

    // The free-space figure belongs to the machine as much as to the path, and
    // spacePath_ alone cannot tell them apart: two hosts both sitting at "/"
    // would match, and the new endpoint would inherit the old one's number
    // without anything looking wrong.
    space_.reset();
    spacePath_.clear();
    spaceQueryPath_.clear();

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

    // The toolbar is glyph-only: these five actions are the ones every file
    // manager draws the same way, so a picture says as much as the word did
    // and says it in every language. Each button's name lives in its tooltip.
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    backBtn_      = MakeIconButton(this, "Back");
    forwardBtn_   = MakeIconButton(this, "Forward");
    upBtn_        = MakeIconButton(this, "Up one level");
    pathCtrl_     = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    refreshBtn_   = MakeIconButton(this, "Refresh");
    newFolderBtn_ = MakeIconButton(this, "New folder");

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
    // Seeded from the model's own default rather than left at the widget's, so
    // the checkbox and the listing cannot start out disagreeing. This is one of
    // the two places to change if the default ever becomes a preference.
    hiddenCheck_->SetValue(term::fs::kDefaultShowHidden);

    filterRow->Add(filterLabel_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
    filterRow->Add(filterCtrl_,  1, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
    filterRow->Add(hiddenCheck_, 0, wxALIGN_CENTER_VERTICAL);
    outer->Add(filterRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    backBtn_->Bind(wxEVT_BUTTON,    [this](wxCommandEvent&) { if (controller_) controller_->GoBack(); });
    forwardBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (controller_) controller_->GoForward(); });
    upBtn_->Bind(wxEVT_BUTTON,      [this](wxCommandEvent&) { if (controller_) controller_->NavigateUp(); });
    refreshBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Reload(); });
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
        // Escape abandons an edit in progress and hands the keyboard back to
        // the listing. Without it Ctrl+L and Ctrl+F are one-way doors: they
        // reach the text fields, and only the mouse comes back out.
        if (evt.GetModifiers() == wxMOD_NONE && evt.GetKeyCode() == WXK_ESCAPE) {
            const wxWindow* const focus = FindFocus();
            if (focus == pathCtrl_) {
                // The field holds an edit that was never committed; the
                // directory actually on screen is what it should say.
                pathCtrl_->ChangeValue(wxString::FromUTF8(CurrentPath()));
                FocusList();
                return;
            }
            if (focus == filterCtrl_) {
                // Nothing to abandon: the filter applies as it is typed, so its
                // text is already the applied state. Clearing it here would
                // discard a view the user can see and undo for themselves.
                FocusList();
                return;
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
    }, kMultiSelect);
    list_->InsertStandardColumns(cfg_.fileExplorerColumnWidths);
    outer->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);

    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,   &FileExplorerPane::OnItemActivated,   this);
    list_->Bind(wxEVT_LIST_COL_CLICK,        &FileExplorerPane::OnColumnClick,     this);
    list_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, &FileExplorerPane::OnContextMenu,     this);
    list_->Bind(wxEVT_LIST_KEY_DOWN,         &FileExplorerPane::OnListKeyDown,     this);
    list_->Bind(wxEVT_LIST_ITEM_SELECTED,    &FileExplorerPane::OnSelectionChanged, this);
    list_->Bind(wxEVT_LIST_ITEM_DESELECTED,  &FileExplorerPane::OnSelectionChanged, this);

    // END_DRAG, not DRAGGING: the latter fires per mouse-move and each one
    // would rewrite config.ini, which is the same reason the frame does not
    // persist on wxEVT_SIZE.
    //
    // Deferred because this event carries the width wx is about to apply, not
    // one it has applied — reading the column here returns the old number.
    // Guarded because the notification then outlives the event, and a pane can
    // be destroyed between the two.
    list_->Bind(wxEVT_LIST_COL_END_DRAG, [this](wxListEvent& evt) {
        auto ctx = guard_.For(this);
        ctx.Post([](FileExplorerPane& pane) {
            if (pane.onColumnWidthsChanged_) pane.onColumnWidthsChanged_();
        });
        evt.Skip();
    });

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
        // Red is the palette's own, so the warning reads as part of the theme
        // rather than painted over it. Normal and bright are both offered
        // because one of the two disappears into the listing background — on a
        // dark scheme it is the dim one, on a light scheme the bright one.
        list_->SetBrokenLinkColour(pickContrasting(bg, toWx(cfg_.ansiColors[kAnsiRed]),
                                                   toWx(cfg_.ansiColors[kAnsiBrightRed])));
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
    if (filterLabel_)  filterLabel_->SetForegroundColour(labelFg);
    if (hiddenCheck_)  hiddenCheck_->SetForegroundColour(labelFg);
    // The status line is the one loose label whose colour is not simply the
    // resting one — it may be mid-message when the theme changes.
    ApplyStatusTone();
    if (endpointChoice_) {
        endpointChoice_->SetBackgroundColour(bg);
        endpointChoice_->SetForegroundColour(fg);
    }

    const wxColour btnBg = toWx(cfg_.uiColors.tileInactive);
    const wxColour btnFg = pickContrasting(btnBg, bg, fg);
    StyleToolButton(backBtn_,      Icon::Back,      btnBg, btnFg);
    StyleToolButton(forwardBtn_,   Icon::Forward,   btnBg, btnFg);
    StyleToolButton(upBtn_,        Icon::Up,        btnBg, btnFg);
    StyleToolButton(refreshBtn_,   Icon::Refresh,   btnBg, btnFg);
    StyleToolButton(newFolderBtn_, Icon::NewFolder, btnBg, btnFg);

    // The glyphs are what the buttons are measured by, so the row has to be
    // re-laid out against the ones just installed rather than the previous set.
    Layout();
    wxPanel::Refresh();
}

// ---------------------------------------------------------------------------
// Controller notifications
// ---------------------------------------------------------------------------

void FileExplorerPane::OnExplorerLoadingChanged(bool loading)
{
    // Only the start of a load is announced here. Its end arrives as a contents
    // change carrying the listing to describe, and repainting the counts twice
    // would put the previous directory's figures on screen for a frame.
    if (loading) SetStatus("Loading...", StatusTone::Busy);
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

    if (focusListOnArrival_ && controller_) {
        focusListOnArrival_ = false;
        const term::fs::DirModel& model = controller_->Model();
        // A partial listing still opened the directory and has rows worth
        // acting on. Only one that produced nothing keeps the keyboard in the
        // path field, where the text that failed is still sitting there to be
        // corrected — which is why it is not re-selected: after a typo, editing
        // is likelier than retyping, and a select-all would destroy it on the
        // next keystroke.
        const bool arrived = !model.HasError() || model.IsPartial();
        // A round trip is long enough for the user to have moved on. Focus is
        // theirs to spend, so it is only taken back from where we left it.
        if (arrived && FindFocus() == pathCtrl_) FocusList();
    }

    UpdateStatus();
    UpdateNavigationState();
    RefreshFreeSpace();
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

void FileExplorerPane::SetStatus(const wxString& text, StatusTone tone)
{
    statusTone_ = tone;
    if (!status_) return;
    status_->SetLabel(text);
    ApplyStatusTone();
}

void FileExplorerPane::ApplyStatusTone()
{
    if (!status_) return;

    // Measured against the pane background the label sits on, exactly as the
    // other loose text is — see ApplyConfig.
    const wxColour paneBg = toWx(cfg_.uiColors.frameBackground);
    const wxColour normal = pickContrasting(paneBg, toWx(cfg_.ansiColors[0]),
                                                    toWx(cfg_.ansiColors[7]));
    status_->SetForegroundColour(StatusToneColour(statusTone_, cfg_, paneBg, normal));
    status_->Refresh();

    // Only when the spinner came or went: the status line is rewritten on every
    // progress callback, and laying the pane out again for each one would be
    // work done for a layout that has not moved.
    if (ApplyToneToSpinner(spinner_, statusTone_)) Layout();
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

    // Only when it describes the directory on screen. A figure for the previous
    // one is worse than none: it looks current, and on a host with several
    // volumes it can be wrong by a whole disk.
    if (space_ && spacePath_ == controller_->CurrentPath()) {
        text += wxString::Format("   -   %s free",
                                 FormatByteSize(space_->availableBytes));
        if (space_->readOnly) text += " (read-only)";
    }

    // Both cases are the same failure differing only in how much of the
    // listing survived it, and both are worth the user's eye: rows that are
    // missing are exactly what a correct-looking count will not tell them.
    StatusTone tone = StatusTone::Normal;
    if (model.IsPartial()) {
        text += "   incomplete: " + DecodeForDisplay(model.Error().message);
        tone = StatusTone::Error;
    } else if (model.HasError()) {
        text = "Error: " + DecodeForDisplay(model.Error().message);
        tone = StatusTone::Error;
    }

    SetStatus(text, tone);
}

void FileExplorerPane::RefreshFreeSpace(bool force)
{
    if (!controller_ || !endpoint_.Valid()) return;

    const std::string path = controller_->CurrentPath();
    if (path.empty()) return;

    // Contents change for reasons that cannot move the volume — a filter
    // keystroke, a re-sort — and each one would otherwise cost a round trip.
    // Only an explicit refresh re-asks about a directory already answered for,
    // or already being asked about.
    if (!force && space_ && spacePath_ == path)  return;
    if (!force && spaceQueryPath_ == path)       return;

    // Dropped before the query rather than after it: whatever is held now
    // describes the directory being left, and leaving it on screen until an
    // answer arrives for the new one is exactly the staleness spacePath_ exists
    // to prevent.
    space_.reset();
    spacePath_.clear();
    spaceQueryPath_ = path;

    auto ctx = guard_.For(this);
    endpoint_.fs->QuerySpace(path, [ctx, path](term::transport::FsSpaceInfo info,
                                               term::transport::FsError err) {
        ctx.Post([path, info, err = std::move(err)](FileExplorerPane& p) mutable {
            // Released whatever the answer was, or a query that failed would
            // suppress every retry for that directory.
            if (p.spaceQueryPath_ == path) p.spaceQueryPath_.clear();

            // A server without the statvfs extension answers Unsupported, which
            // is ordinary rather than an error worth reporting — the status line
            // simply carries no figure. Nothing is shown for a genuine failure
            // either: the user asked to see a directory, not to be told about a
            // volume query they never made.
            if (err.Failed()) return;

            p.space_     = info;
            p.spacePath_ = path;
            p.UpdateStatus();
        });
    });
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
        // The mode travels with the item so a copy can reproduce it without a
        // second round trip; the listing has already told us.
        items.push_back({controller_->PathOf(row), e.name, e.isDir, e.isSymlink,
                         e.size, e.mode});
    }
    return items;
}

void FileExplorerPane::Reload()
{
    if (controller_) controller_->Refresh();
    // Forced: an explicit reload is the one moment the user has asked for
    // current information, and it is also when the volume is most likely to
    // have moved — a transfer into this directory has just finished.
    RefreshFreeSpace(true);
}

void FileExplorerPane::ReloadFocusing(std::string focusName)
{
    pendingFocusName_ = std::move(focusName);
    Reload();
}

void FileExplorerPane::GoOffline(const wxString& reason)
{
    controller_.reset();
    deleter_.reset();
    endpoint_.fs = nullptr;
    if (list_) { list_->SetItemCount(0); list_->Refresh(); }
    // A pane with no filesystem behind it still looks like a pane. The reason
    // is the only thing distinguishing an empty directory from a dead one.
    SetStatus(reason, StatusTone::Error);
    UpdateNavigationState();
    if (onStateChanged_) onStateChanged_();
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void FileExplorerPane::OnGo(wxCommandEvent&)
{
    if (!controller_) return;
    // Only Enter in the path field reaches here, so the request came from the
    // keyboard and the keyboard should end up somewhere useful when it lands.
    focusListOnArrival_ = true;
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

    // Guarded like every other asynchronous call here. Activating a symlink
    // costs a stat round trip, and the pane can be repointed or torn down
    // before it answers.
    auto ctx = guard_.For(this);
    controller_->Activate(row,
        [ctx](term::fs::ActivationResult result, std::string path,
              term::transport::FsError err) {
            ctx.Post([result, path = std::move(path), err = std::move(err)](
                         FileExplorerPane& p) mutable {
                switch (result) {
                    case term::fs::ActivationResult::Navigated:
                        break;
                    case term::fs::ActivationResult::IsFile:
                        if (p.onOpenInEditor_)
                            p.onOpenInEditor_(p.endpoint_.sessionId, std::move(path));
                        break;
                    case term::fs::ActivationResult::Failed:
                        p.SetStatus(wxString::Format("Cannot open '%s': %s",
                                                     DecodeForDisplay(path),
                                                     DecodeForDisplay(err.message)),
                                    StatusTone::Error);
                        break;
                }
            });
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

RemoteFileListCtrl::ColumnWidths FileExplorerPane::ColumnWidths() const
{
    return list_ ? list_->CurrentColumnWidths()
                 : kDefaultFileExplorerColumnWidths;
}

void FileExplorerPane::SetColumnWidths(
    const RemoteFileListCtrl::ColumnWidths& widths)
{
    if (list_) list_->ApplyColumnWidths(widths);
}

void FileExplorerPane::OnListKeyDown(wxListEvent& evt)
{
    const size_t row = static_cast<size_t>(evt.GetIndex());
    switch (evt.GetKeyCode()) {
        case WXK_DELETE:    DeleteSelection(); break;
        case WXK_F2:        RenameRow(row);    break;
        case WXK_F5:        Reload();          break;
        // Backspace goes up a level, as in every file manager.
        case WXK_BACK:      if (controller_) controller_->NavigateUp(); break;
        default:            evt.Skip(); break;
    }
}

bool FileExplorerPane::IsRowSelected(size_t row) const
{
    return list_ && list_->GetItemState(static_cast<long>(row),
                                        wxLIST_STATE_SELECTED) != 0;
}

void FileExplorerPane::SelectOnlyRow(size_t row)
{
    if (!list_) return;

    long index = -1;
    while ((index = list_->GetNextItem(index, wxLIST_NEXT_ALL,
                                       wxLIST_STATE_SELECTED)) != wxNOT_FOUND)
        list_->SetItemState(index, 0, wxLIST_STATE_SELECTED);

    const auto item = static_cast<long>(row);
    list_->SetItemState(item, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                        wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
}

void FileExplorerPane::OnContextMenu(wxListEvent& evt)
{
    if (!controller_) return;
    const auto row = static_cast<size_t>(evt.GetIndex());
    if (row >= controller_->Model().VisibleCount()) return;

    // Every item in this menu has to act on the row the user pointed at. Most
    // re-resolve it by name below; Delete reads the *selection*, because
    // deleting several at once is the case worth having. Those two agree only
    // if the click has made the row the selection — and wxListCtrl does not do
    // that on its own, so a right-click on an unselected row would otherwise
    // offer to delete whatever happened to be picked elsewhere in the listing,
    // under a menu whose every other entry named the file under the cursor.
    //
    // A row already in the selection is left alone: right-clicking one of
    // several picked files means "all of these", not "just this one".
    if (!IsRowSelected(row)) SelectOnlyRow(row);

    // Directory-like, so a link known to lead to a directory is treated as the
    // directory the row already claims it is.
    const bool isDir = controller_->Model().IsDirectoryLike(row);
    // The name, not the index: PopupMenu below runs a nested event loop, so a
    // listing that refreshes while the menu is open would leave the index
    // pointing at a different file than the one under the cursor.
    const std::string name = controller_->Model().At(row).name;
    const size_t selectionCount = SelectedItems().size();

    wxMenu menu;
    auto* editItem   = menu.Append(wxID_ANY, "Open in Editor");
    auto* copyItem   = menu.Append(wxID_ANY, "Copy Path");
    menu.AppendSeparator();
    auto* newItem    = menu.Append(wxID_ANY, "New Folder...");
    auto* renameItem = menu.Append(wxID_ANY, "Rename/Move...\tF2");
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

    // Each row action re-finds its target by name when it runs, and does
    // nothing if the file has since gone.
    const auto bindRowAction = [this, &menu, &name](wxMenuItem* item,
                                                    std::function<void(size_t)> action) {
        menu.Bind(wxEVT_MENU, [this, name, action = std::move(action)](wxCommandEvent&) {
            const size_t target = RowForName(name);
            if (target != kNoRow) action(target);
        }, item->GetId());
    };

    bindRowAction(editItem,   [this](size_t r) { EditRow(r); });
    bindRowAction(copyItem,   [this](size_t r) { CopyPathOf(r); });
    bindRowAction(renameItem, [this](size_t r) { RenameRow(r); });
    bindRowAction(propsItem,  [this](size_t r) { ShowPropertiesFor(r); });

    // These two read live state rather than a row, so there is nothing to
    // re-resolve.
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { NewFolder(); }, newItem->GetId());
    menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) { DeleteSelection(); }, deleteItem->GetId());

    PopupMenu(&menu);
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

size_t FileExplorerPane::RowForName(const std::string& name) const
{
    if (!controller_) return kNoRow;
    const size_t row = controller_->Model().IndexOfName(name);
    return row < controller_->Model().VisibleCount() ? row : kNoRow;
}

bool FileExplorerPane::RequireLive()
{
    if (controller_) return true;
    wxMessageBox("This pane is no longer connected.",
                 "File Explorer", wxOK | wxICON_INFORMATION, this);
    return false;
}

void FileExplorerPane::ReportFailure(const wxString& what,
                                     const term::transport::FsError& err)
{
    wxMessageBox(what + " failed:\n\n" + DecodeForDisplay(err.message),
                 "File Explorer", wxOK | wxICON_ERROR, this);
}

void FileExplorerPane::AfterWrite(const wxString& what,
                                  const term::transport::FsError& err,
                                  std::string focusName)
{
    if (err.Failed()) {
        ReportFailure(what, err);
        return;
    }
    ReloadFocusing(std::move(focusName));
}

void FileExplorerPane::AfterDelete(const term::transport::FsError& err)
{
    if (err.Failed()) ReportFailure("Delete", err);
    // Unconditionally, and after the message box rather than instead of it: a
    // delete that stopped partway has taken files the listing is still showing,
    // and leaving them on screen invites the user to act on rows that are gone.
    Reload();
}

void FileExplorerPane::EditRow(size_t row)
{
    if (!controller_ || !onOpenInEditor_) return;
    if (row >= controller_->Model().VisibleCount()) return;
    if (controller_->Model().IsDirectoryLike(row)) return;
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

    // The dialog pumped events, so the session may have ended while it was open.
    if (!RequireLive()) return;

    auto ctx = guard_.For(this);
    controller_->SetEntryPermissions(name, dlg.SelectedMode(),
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

    // The dialog pumped events, so the session may have ended while it was open.
    if (!RequireLive()) return;

    // The name only. Where it lands is the controller's business, which is what
    // stops a navigation completing during the dialog from putting the folder
    // somewhere the user was not looking.
    const std::string name = dlg.GetValue().Trim().Trim(false).ToStdString();

    auto ctx = guard_.For(this);
    controller_->CreateDirectory(name,
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

    wxTextEntryDialog dlg(this,
                          "New name, or a path to move it to:",
                          "Rename or Move", DecodeForDisplay(oldName));
    if (dlg.ShowModal() != wxID_OK) return;

    // The dialog pumped events, so the session may have ended while it was open.
    if (!RequireLive()) return;

    const std::string destination = dlg.GetValue().Trim().Trim(false).ToStdString();
    if (destination == oldName) return;   // nothing asked for, nothing to report

    // The subject is a name, never a path: the controller anchors it to the
    // directory it is showing, so a navigation completing while the dialog was
    // open cannot make this act on a different file. The destination is free
    // to be a path — resolving it is the controller's job, as is refusing to
    // land on something that already exists.
    //
    // Focus lands on the destination's own leaf, which after a move to another
    // directory is simply not in this listing; the reload then keeps whatever
    // selection it can, which is the right outcome for a row that has left.
    const std::string focus = term::fs::path::Leaf(destination);

    auto ctx = guard_.For(this);
    controller_->RenameEntry(oldName, destination,
        [ctx, focus](term::transport::FsError err) {
            ctx.Post([focus, err = std::move(err)](FileExplorerPane& p) mutable {
                p.AfterWrite("Rename", err, focus);
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

    // The walk that costs a round trip per directory, before any confirmation
    // has been asked for: nothing else on screen moves while it runs.
    SetStatus("Examining " + label + "...", StatusTone::Busy);

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
                                                         done, total),
                                        StatusTone::Busy);
                        });
                    },
                    [inner](term::transport::FsError err) {
                        inner.Post([err = std::move(err)](FileExplorerPane& q) mutable {
                            q.AfterDelete(err);
                        });
                    });
            });
        });
}

} // namespace ui
