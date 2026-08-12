#include "ui/FileExplorerFrame.h"

#include "fs/RemotePath.h"
#include "ui/AppIconBundle.h"
#include "ui/ColorUtils.h"
#include "ui/ConflictDialog.h"
#include "ui/StringUtils.h"
#include "ui/ToolButton.h"
#include "ui/TransferPanel.h"

#include <algorithm>
#include <filesystem>

#include <wx/app.h>
#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

namespace ui {

namespace {

// The width is a *one-pane* width throughout: Transfer mode derives its own
// width by doubling it, so the two modes cannot drift apart.
constexpr int kInitialWidth    = 720;
constexpr int kInitialHeight   = 700;
constexpr int kMinWidth        = 620;
constexpr int kMinHeight       = 420;
constexpr int kSplitterMinPane = 280;
// Copy direction is carried by the button's arrow glyph, so the label only has
// to say what happens — and, once a destination is known, where it lands.
constexpr char kCopyLabel[]         = "Copy";
constexpr char kCopyToLabelFormat[] = "Copy to %s";
// The margin the outer sizer puts around the splitter and the transfer panel.
// FrameChromeWidth() derives the frame's non-listing width from the horizontal
// pair, so every sizer call that contributes to it must use this constant
// rather than repeating the number.
constexpr int kOuterMargin     = 4;
// wx reads a sash position of zero as "down the middle".
constexpr int kCentredSash     = 0;

// The controls row: its own margin either side, the space after the mode
// button, the margin between the two copy buttons, and how close the pair may
// come to the mode button before it stops chasing the sash.
constexpr int kControlsMargin  = 6;
constexpr int kModeButtonGap   = 16;
constexpr int kCopyButtonGap   = 8;
constexpr int kMinCopyPairGap  = 8;

// "naFX" — *not another File eXplorer*, in the spirit of naTE itself. The
// explorer gets its own name rather than the application's because a taskbar
// otherwise shows "naTE 1:2" beside "naTE root@prod", where a shared prefix
// has to be read past before the two are told apart.
//
// Nothing else is renamed: this window belongs to naTE and still says so
// where it counts — it wears naTE's icon, and the desktop groups it under
// naTE's WM_CLASS. Menu entries and message boxes stay "File Explorer", which
// is how a user looks the feature up.
//
// The prefix is kept short for the same reason it is no longer "naTE: Files":
// the endpoints are the only thing distinguishing two explorer windows, and
// they have to survive the truncation a taskbar applies.
constexpr const char* kTitleApp         = "naFX";
constexpr const char* kTitleEndpointSep = " ";
constexpr const char* kTitlePairSep     = " <-> ";

// How this machine names itself as an endpoint. Every place that builds a local
// endpoint uses it, so the choice column, the copy buttons, the window title and
// the transfer queue all say the same thing about the same machine.
constexpr char kLocalEndpointLabel[] = "This computer";

// Where the local pane opens. The user's home directory is the only defensible
// default: the process working directory is wherever naTE happened to be
// launched from, which is rarely anywhere the user wants.
std::string DefaultLocalPath()
{
    if (const char* home = std::getenv("HOME")) return home;
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::string("/") : cwd.string();
}

// The local adapter holds no state, so one instance serves every window. A
// per-frame member would have to outlive the child panes that reference it,
// and wx destroys a frame's members before its children.
term::transport::LocalFileSystem& SharedLocalFileSystem()
{
    static term::transport::LocalFileSystem instance;
    return instance;
}

term::fs::TransferEndpoint ToTransferEndpoint(const PaneEndpoint& e)
{
    return {e.fs, e.label};
}

} // namespace

FileExplorerFrame::FileExplorerFrame(wxWindow* parent,
                                     term::session::SessionId sessionId,
                                     term::session::SessionManager& sm,
                                     const AppConfig& cfg,
                                     OpenInEditorFn onOpenInEditor)
    // A bare title only until the panes exist; UpdateTitle names the endpoints
    // as soon as there are panes to read them from.
    : wxFrame(parent, wxID_ANY, kTitleApp,
              wxDefaultPosition,
              wxSize(cfg.fileExplorerWidth  > 0 ? cfg.fileExplorerWidth  : kInitialWidth,
                     cfg.fileExplorerHeight > 0 ? cfg.fileExplorerHeight : kInitialHeight))
    , sessionId_(sessionId)
    , sm_(sm)
    , cfg_(cfg)
{
    // Two fields: the first carries one-shot messages, the second the live
    // queue. Sharing one would let per-tick progress stamp on a message the
    // user has not read yet.
    SetIcons(AppIconBundle());

    status_ = CreateStatusBar(2);
    const int widths[2] = {-1, 260};
    status_->SetStatusWidths(2, widths);
    Bind(wxEVT_DESTROY, &FileExplorerFrame::OnDestroy, this);

    // Sessions open and close in the main window while this one is up, so the
    // endpoint lists are re-read whenever the user comes back to it.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& evt) {
        if (evt.GetActive()) RefreshEndpointChoices();
        evt.Skip();
    });

    BuildLayout(std::move(onOpenInEditor));
    SetMinSize(wxSize(kMinWidth, kMinHeight));
    ApplyConfig(cfg_);

    UpdateTransferButtons();
    UpdateTitle();

    // Deliberately not bound to wxEVT_SIZE: a single window drag emits dozens
    // of those, and each one would rewrite config.ini. The shape is captured
    // when the sash settles, when the mode changes, and on close — which is
    // when the user has finished choosing it.
    splitter_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED,
                    [this](wxSplitterEvent& evt) {
                        AlignCopyButtonsToSash(evt.GetSashPosition());
                        // Dragging the sash changes the leading pane's width,
                        // which is the width the window remembers. wx emits
                        // this during a programmatic split/unsplit too, where
                        // the shape is mid-flight and must not be recorded.
                        if (applyingMode_ || !splitter_->IsSplit()) { evt.Skip(); return; }
                        PersistGeometry();
                        evt.Skip();
                    });

    // Live update means the sash is already under the cursor before it settles;
    // the buttons follow it there rather than jumping when the drag ends. The
    // event carries the position wx is about to apply, which the splitter has
    // not been told about yet.
    splitter_->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING,
                    [this](wxSplitterEvent& evt) {
                        AlignCopyButtonsToSash(evt.GetSashPosition());
                        evt.Skip();
                    });

    // Sash gravity moves the sash as the frame is resized. Deferred rather than
    // handled inline: this runs before the frame lays out, so the splitter has
    // not yet applied the gravity that this alignment depends on.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        CallAfter([this] { AlignCopyButtonsToSash(); });
        evt.Skip();
    });
}

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------

PaneEndpoint FileExplorerFrame::LocalEndpoint() const
{
    return {kLocalEndpointLabel, &SharedLocalFileSystem(), 0, DefaultLocalPath()};
}

PaneEndpoint FileExplorerFrame::EndpointForSession(term::session::SessionId id) const
{
    PaneEndpoint endpoint;
    endpoint.fs        = sm_.GetRemoteFileSystem(id);
    endpoint.sessionId = id;
    endpoint.label     = sm_.GetRemoteDescription(id);
    if (endpoint.label.empty()) endpoint.label = "Session " + std::to_string(id);
    // Start where that session's shell is, when it is known.
    endpoint.defaultPath = sm_.GetCurrentWorkingDir(id);
    if (endpoint.defaultPath.empty()) endpoint.defaultPath = ".";
    return endpoint;
}

std::vector<PaneEndpoint> FileExplorerFrame::AvailableEndpoints() const
{
    std::vector<PaneEndpoint> endpoints;
    endpoints.push_back(LocalEndpoint());
    for (const term::session::SessionId id : sm_.GetSessionIds()) {
        if (!sm_.SupportsFileTransfer(id)) continue;
        endpoints.push_back(EndpointForSession(id));
    }
    return endpoints;
}

void FileExplorerFrame::RefreshEndpointChoices()
{
    if (leftPane_)  leftPane_->RefreshEndpointChoices();
    if (rightPane_) rightPane_->RefreshEndpointChoices();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void FileExplorerFrame::SetOnOpenInEditor(OpenInEditorFn cb)
{
    if (leftPane_)  leftPane_->SetOnOpenInEditor(cb);
    if (rightPane_) rightPane_->SetOnOpenInEditor(std::move(cb));
}

void FileExplorerFrame::BuildLayout(OpenInEditorFn onOpenInEditor)
{
    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto provider = [this] { return AvailableEndpoints(); };

    splitter_ = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3D);
    splitter_->SetMinimumPaneSize(kSplitterMinPane);
    splitter_->SetSashGravity(0.5);

    // The pane the window was opened for leads; the second starts on this
    // computer, which is what a transfer most often involves.
    leftPane_  = new FileExplorerPane(splitter_, cfg_, provider,
                                      EndpointForSession(sessionId_));
    rightPane_ = new FileExplorerPane(splitter_, cfg_, provider, LocalEndpoint());

    leftPane_->SetOnOpenInEditor(onOpenInEditor);
    rightPane_->SetOnOpenInEditor(onOpenInEditor);

    splitter_->Initialize(leftPane_);
    rightPane_->Hide();
    outer->Add(splitter_, 1, wxEXPAND | wxALL, kOuterMargin);

    queue_ = std::make_unique<term::fs::TransferQueue>(
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); });
    queue_->SetListener(this);
    queue_->SetConflictPrompt([this](const term::fs::TransferJob& job, auto respond) {
        ConflictDialog dlg(this, cfg_, job);
        dlg.ShowModal();
        respond(dlg.Resolution(), dlg.ApplyToAll());
    });

    controls_   = new wxBoxSizer(wxHORIZONTAL);
    // These three keep their words. A copy is not undoable once the bytes land
    // and the destination is named in the label, so the glyph here reinforces
    // the direction rather than standing in for the sentence.
    modeBtn_    = new wxButton(this, wxID_ANY, "Transfer Mode");
    toRightBtn_ = new wxButton(this, wxID_ANY, kCopyLabel);
    toLeftBtn_  = new wxButton(this, wxID_ANY, kCopyLabel);

    controls_->Add(modeBtn_, 0, wxRIGHT, kModeButtonGap);
    // Not a stretch spacer: the pair is positioned against the sash, which is
    // wherever the user left it, rather than centred in what is left of the row.
    copyPairGap_ = controls_->AddSpacer(0);
    controls_->Add(toRightBtn_, 0, wxRIGHT, kCopyButtonGap);
    controls_->Add(toLeftBtn_,  0);
    outer->Add(controls_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kControlsMargin);

    transfers_ = new TransferPanel(this, cfg_, *queue_);
    transfers_->SetOnCancelJob([this](term::fs::JobId id) { queue_->CancelJob(id); });
    transfers_->SetOnCancelAll([this] { queue_->CancelAll(); });
    transfers_->SetOnClearFinished([this] {
        queue_->ClearFinished();
        transfers_->RefreshFromQueue();
    });
    outer->Add(transfers_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, kOuterMargin);

    modeBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        SetMode(mode_ == FileExplorerMode::Transfer ? FileExplorerMode::Explore
                                                    : FileExplorerMode::Transfer);
    });
    toRightBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        CopyBetweenPanes(leftPane_, rightPane_);
    });
    toLeftBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        CopyBetweenPanes(rightPane_, leftPane_);
    });

    const auto onState = [this] { UpdateTransferButtons(); UpdateTitle(); };
    leftPane_->SetOnStateChanged(onState);
    rightPane_->SetOnStateChanged(onState);
    leftPane_->SetOnEndpointChanged(onState);
    rightPane_->SetOnEndpointChanged(onState);

    leftPane_->SetOnLocalFilesDropped([this](std::vector<std::string> paths) {
        OnFilesDropped(leftPane_, std::move(paths));
    });
    rightPane_->SetOnLocalFilesDropped([this](std::vector<std::string> paths) {
        OnFilesDropped(rightPane_, std::move(paths));
    });

    SetSizer(outer);

    // Born in Explore mode: the panes are already built that way, but the
    // transfer chrome has to be hidden or the window would open as Explore
    // panes wearing Transfer controls.
    controls_->Show(toRightBtn_, false);
    controls_->Show(toLeftBtn_,  false);
    outer->Show(transfers_, false);

    // Lay out now rather than at first paint: the owner may switch the window
    // into Transfer mode before showing it, and the mode arithmetic measures
    // the pane it is doubling.
    Layout();
}

void FileExplorerFrame::SetMode(FileExplorerMode mode)
{
    if (mode == mode_) return;
    mode_ = mode;
    ApplyMode();
}

void FileExplorerFrame::ApplyMode()
{
    const bool transfer = mode_ == FileExplorerMode::Transfer;
    const int  panes    = transfer ? 2 : 1;

    // One paint for the whole reshape. Without this the split, the panel
    // appearing and the relayout each draw separately, which reads as a flicker.
    Freeze();
    applyingMode_ = true;

    // The shape the listing has right now, in both axes. The width target is
    // arithmetic — a pane is a pane — but the height the queue panel will take
    // is the sizer's business, so it is measured afterwards rather than
    // predicted here.
    const int listingHeight = splitter_ ? splitter_->GetSize().y : 0;
    const int frameHeight   = GetSize().y;
    const int targetWidth   = FrameWidthForPanes(panes);

    // Lowered before the frame shrinks, or it would block its own shrink. The
    // mode's real floor is set at the end, once the queue's height is known.
    SetMinSize(wxSize(MinFrameWidthForPanes(panes), kMinHeight));

    if (transfer) {
        // Unsplit hid it; wx will not show it again on its own.
        rightPane_->Show();
        splitter_->SplitVertically(leftPane_, rightPane_, kCentredSash);
        SetFrameSize(targetWidth, frameHeight);
        // Sash gravity keeps a centred split centred as the frame grows, but
        // the grow may land after this call on some platforms. Centring again
        // makes the outcome independent of that ordering.
        CentreSash();
        // The listing may have gone stale while hidden, and the endpoint list
        // certainly may have.
        rightPane_->RefreshEndpointChoices();
        rightPane_->Reload();
    } else {
        splitter_->Unsplit(rightPane_);
        SetFrameSize(targetWidth, frameHeight);
    }

    // Tell the *sizer*, not just the window: a sizer item keeps its own show
    // flag, and hiding the window alone leaves its space reserved as an empty
    // strip.
    controls_->Show(toRightBtn_, transfer);
    controls_->Show(toLeftBtn_,  transfer);
    GetSizer()->Show(transfers_, transfer);

    modeBtn_->SetLabel(transfer ? "Explore Mode" : "Transfer Mode");

    UpdateTransferButtons();
    UpdateTitle();
    Layout();

    // Give the listing back exactly what the queue panel took, or reclaim
    // exactly what it freed. Measured, not predicted: the height the sizer
    // grants that panel is not a figure the panel itself can be asked for.
    RestoreListingHeight(listingHeight);

    // The frame moved by however much that turned out to be — the figure
    // Explore mode has to subtract to describe itself.
    if (transfer) transferHeightDelta_ = GetSize().y - frameHeight;

    SetMinSize(wxSize(MinFrameWidthForPanes(panes),
                      kMinHeight + (transfer ? transferHeightDelta_ : 0)));

    applyingMode_ = false;
    Thaw();
    PersistGeometry();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// The window is measured in panes. Explore mode is one pane wide, Transfer
// mode two panes plus a sash, and a mode switch is nothing but that arithmetic
// applied to the pane the user can already see. No sash position or window
// size is carried across a switch, so there is nothing to go stale.

int FileExplorerFrame::FrameChromeWidth() const
{
    // Window border plus the sizer margin either side of the splitter. Both
    // are theme-dependent, so they are measured off the live window; the
    // fallback covers the moment before the first layout has run.
    if (splitter_) {
        const int splitterWidth = splitter_->GetSize().x;
        if (splitterWidth > 0) return GetSize().x - splitterWidth;
    }
    return (GetSize().x - GetClientSize().x) + 2 * kOuterMargin;
}

int FileExplorerFrame::PaneWidth() const
{
    if (!splitter_) return 0;
    // While split, the leading pane is the unit — so a sash the user has
    // dragged is carried over exactly when the second pane goes away, rather
    // than being averaged into something they did not choose.
    if (splitter_->IsSplit() && leftPane_) return leftPane_->GetSize().x;
    return splitter_->GetSize().x;
}

PaneMetrics FileExplorerFrame::Metrics() const
{
    return {FrameChromeWidth(),
            splitter_ ? splitter_->GetSashSize() : 0,
            kSplitterMinPane};
}

int FileExplorerFrame::FrameWidthForPanes(int panes) const
{
    return ui::FrameWidthForPanes(panes, PaneWidth(), Metrics());
}

int FileExplorerFrame::MinFrameWidthForPanes(int panes) const
{
    // Two panes need room for two: without this the frame minimum would let
    // the user squeeze the window until wx started stealing width from one
    // pane to keep the other above its own minimum.
    return ui::MinFrameWidthForPanes(panes, Metrics(), kMinWidth);
}

void FileExplorerFrame::RestoreListingHeight(int wanted)
{
    // Called after the reshape has been laid out, so the drift below is what
    // the queue panel actually cost — the panel's own best size excludes the
    // minimum it sets on itself, so asking it would understate the answer and
    // leave the listings a different height in each mode.
    if (!splitter_ || wanted <= 0) return;

    const int drift = wanted - splitter_->GetSize().y;
    if (drift == 0) return;

    SetFrameSize(GetSize().x, GetSize().y + drift);
    Layout();
}

void FileExplorerFrame::SetFrameSize(int width, int height)
{
    // A maximised or full-screen window has no size of its own to change; the
    // panes simply share whatever the screen gives them.
    if (IsMaximized() || IsFullScreen()) return;

    const int index = wxDisplay::GetFromWindow(this);
    const wxRect area =
        wxDisplay(index == wxNOT_FOUND ? 0u : static_cast<unsigned>(index))
            .GetClientArea();

    width  = std::min(width,  area.GetWidth());
    height = std::min(height, area.GetHeight());
    if (width == GetSize().x && height == GetSize().y) return;
    SetSize(width, height);

    // Growing from near an edge would otherwise push the pane it grew for off
    // the screen.
    const wxPoint at = GetPosition();
    const int overhangX = at.x + width  - (area.GetX() + area.GetWidth());
    const int overhangY = at.y + height - (area.GetY() + area.GetHeight());
    if (overhangX > 0 || overhangY > 0)
        Move(std::max(area.GetX(), at.x - std::max(0, overhangX)),
             std::max(area.GetY(), at.y - std::max(0, overhangY)));
}

void FileExplorerFrame::CentreSash()
{
    if (!splitter_ || !splitter_->IsSplit()) return;
    splitter_->SetSashPosition(
        (splitter_->GetClientSize().x - splitter_->GetSashSize()) / 2);
}

void FileExplorerFrame::AlignCopyButtonsToSash()
{
    if (splitter_) AlignCopyButtonsToSash(splitter_->GetSashPosition());
}

void FileExplorerFrame::AlignCopyButtonsToSash(int sashPosition)
{
    // Explore mode has neither a sash nor the buttons that follow it.
    if (!copyPairGap_ || !splitter_ || !splitter_->IsSplit()) return;

    // Sash position is relative to the splitter's client area, and the row is
    // laid out in the frame's — so the splitter's own offset has to be added.
    const int sashCentre = splitter_->GetPosition().x + sashPosition
                         + splitter_->GetSashSize() / 2;

    // Effective minimums, not current sizes: the labels name their destination
    // and change width as the panes are repointed, and this runs before the
    // row has been laid out at the new widths.
    const ControlsRowMetrics metrics{
        kControlsMargin,
        GetClientSize().x - kControlsMargin,
        modeBtn_->GetEffectiveMinSize().x + kModeButtonGap,
        toRightBtn_->GetEffectiveMinSize().x,
        toLeftBtn_->GetEffectiveMinSize().x,
        kCopyButtonGap,
        kMinCopyPairGap};

    const int gap = LeadingGapForSashAlignedPair(sashCentre, metrics);
    // A live sash drag emits these by the dozen and most land on the same
    // pixel, so the relayout is skipped unless the answer actually moved.
    if (gap == copyPairGap_->GetMinSize().x) return;

    copyPairGap_->AssignSpacer(gap, 0);
    // The row only, not the frame: this fires mid-drag, and re-laying out the
    // splitter while it is applying a sash position it has not committed yet
    // would have the two fighting over the same pixel.
    controls_->Layout();
}

void FileExplorerFrame::PersistGeometry()
{
    // Null once the owner has gone; the window may still be torn down after it.
    if (!onGeometryChanged_ || !splitter_) return;
    // A maximised or minimised window's size is not what the user chose, so it
    // must not overwrite the size they did choose.
    if (IsMaximized() || IsIconized()) return;

    // Always the Explore-mode shape, whatever mode is showing: that is what a
    // window opens at, and Transfer mode derives its own from it. Recording the
    // two-pane width would reopen an Explore window at twice the size.
    const int height = GetSize().y - (mode_ == FileExplorerMode::Transfer
                                          ? transferHeightDelta_ : 0);
    onGeometryChanged_(FrameWidthForPanes(1), height);
}

void FileExplorerFrame::OnFilesDropped(FileExplorerPane* target,
                                       std::vector<std::string> paths)
{
    if (!queue_ || !target || paths.empty()) return;

    if (!target->IsLive()) return;
    if (target->CurrentEndpoint().IsLocalDisk()) {
        // Dropping desktop files onto the local pane would be a local copy,
        // which this window does not do. Say so rather than doing nothing.
        wxMessageBox("Dropped files can only be sent to a remote session.\n\n"
                     "Point this pane at a session, or drop onto one that "
                     "already is.",
                     "Copy", wxOK | wxICON_INFORMATION, this);
        return;
    }

    const term::fs::TransferEndpoint source{&SharedLocalFileSystem(),
                                            kLocalEndpointLabel};
    const auto destination = ToTransferEndpoint(target->CurrentEndpoint());
    const std::string destDir = target->CurrentPath();

    size_t queued = 0;
    for (const std::string& path : paths) {
        std::error_code ec;
        const bool isDir = std::filesystem::is_directory(path, ec);
        if (ec) continue;

        FileExplorerPane::Item item;
        item.path = path;
        item.name = std::filesystem::path(path).filename().string();
        item.isDir = isDir;
        if (!isDir) {
            const auto size = std::filesystem::file_size(path, ec);
            item.size = ec ? 0 : static_cast<uint64_t>(size);
        }
        QueueOne(item, source, destination, destDir);
        ++queued;
    }

    if (queued) {
        destinationDirty_ = true;
        if (status_)
            status_->SetStatusText(
                wxString::Format("Queued %zu dropped item%s for %s",
                                 queued, queued == 1 ? "" : "s",
                                 DecodeForDisplay(destination.label)));
    }
}

void FileExplorerFrame::ApplyConfig(const AppConfig& cfg)
{
    cfg_ = cfg;
    SetBackgroundColour(toWx(cfg_.uiColors.frameBackground));

    if (leftPane_)  leftPane_->ApplyConfig(cfg_);
    if (rightPane_) rightPane_->ApplyConfig(cfg_);
    if (transfers_) transfers_->ApplyConfig(cfg_);

    const wxColour btnBg = toWx(cfg_.uiColors.tileInactive);
    const wxColour btnFg = pickContrasting(btnBg, toWx(cfg_.ansiColors[0]),
                                           toWx(cfg_.ansiColors[7]));
    StyleToolButton(modeBtn_,    Icon::SplitPanes, btnBg, btnFg);
    // The arrow sits on the side it points to, so the button reads as the
    // movement it performs rather than as a label with decoration.
    StyleToolButton(toRightBtn_, Icon::CopyRight,  btnBg, btnFg, wxRIGHT);
    StyleToolButton(toLeftBtn_,  Icon::CopyLeft,   btnBg, btnFg, wxLEFT);

    Layout();
    // Glyphs widen the buttons, so the alignment is re-derived after styling.
    AlignCopyButtonsToSash();
    wxFrame::Refresh();
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

void FileExplorerFrame::UpdateTransferButtons()
{
    const bool paired = mode_ == FileExplorerMode::Transfer &&
                        leftPane_ && rightPane_ &&
                        leftPane_->IsLive() && rightPane_->IsLive();

    if (!paired) {
        if (toRightBtn_) { toRightBtn_->Enable(false); toRightBtn_->SetLabel(kCopyLabel); }
        if (toLeftBtn_)  { toLeftBtn_->Enable(false);  toLeftBtn_->SetLabel(kCopyLabel); }
        return;
    }

    // Label the buttons with where the bytes would actually land, so the
    // direction is unambiguous once a pane can point anywhere.
    toRightBtn_->SetLabel(
        wxString::Format(kCopyToLabelFormat,
                         DecodeForDisplay(rightPane_->CurrentEndpoint().label)));
    toLeftBtn_->SetLabel(
        wxString::Format(kCopyToLabelFormat,
                         DecodeForDisplay(leftPane_->CurrentEndpoint().label)));

    toRightBtn_->Enable(!leftPane_->SelectedItems().empty());
    toLeftBtn_->Enable(!rightPane_->SelectedItems().empty());
    Layout();
    // A relabelled button is a differently sized one, and the seam has to stay
    // on the sash across the change.
    AlignCopyButtonsToSash();
}

void FileExplorerFrame::UpdateTitle()
{
    // Read off the panes at the moment of the change rather than caching what
    // the window was opened for: either pane may be repointed at any endpoint,
    // from the endpoint choice, a mode switch or a session ending, and a
    // remembered description would name the wrong machine after any of them.
    if (!leftPane_) return;

    // Endpoint labels, not paths. The endpoint is what distinguishes two
    // explorer windows in a taskbar, whereas the path changes on every
    // double-click and is already on screen in the pane's own path control.
    wxString title = wxString(kTitleApp) + kTitleEndpointSep +
                     DecodeForDisplay(leftPane_->CurrentEndpoint().label);

    if (mode_ == FileExplorerMode::Transfer && rightPane_) {
        title += kTitlePairSep;
        title += DecodeForDisplay(rightPane_->CurrentEndpoint().label);
    }

    // The state callback also fires on every navigation, and the title only
    // moves when an endpoint or the mode does.
    if (GetTitle() != title) SetTitle(title);
}

void FileExplorerFrame::QueueOne(const FileExplorerPane::Item& item,
                                 const term::fs::TransferEndpoint& source,
                                 const term::fs::TransferEndpoint& destination,
                                 const std::string& destinationDir)
{
    const std::string dest = term::fs::path::Join(destinationDir, item.name);

    // A symlink is copied as whatever it points at rather than reproduced as a
    // link: the target may not exist on the other side, and a dangling link is
    // a worse outcome than a real file.
    if (item.isDir && !item.isSymlink) {
        queue_->EnqueueTree(source, item.path, destination, dest,
            [this, name = item.name](term::transport::FsError err) {
                if (err.Failed() && status_)
                    status_->SetStatusText(
                        wxString::Format("Could not fully read '%s': %s",
                                         DecodeForDisplay(name),
                                         DecodeForDisplay(err.message)));
            });
        return;
    }

    queue_->Enqueue(source, item.path, destination, dest, item.size);
}

void FileExplorerFrame::CopyBetweenPanes(FileExplorerPane* from, FileExplorerPane* to)
{
    if (!queue_ || !from || !to || !from->IsLive() || !to->IsLive()) return;

    const auto items = from->SelectedItems();
    if (items.empty()) return;

    const auto source      = ToTransferEndpoint(from->CurrentEndpoint());
    const auto destination = ToTransferEndpoint(to->CurrentEndpoint());
    const std::string destDir = to->CurrentPath();

    if (source.IsLocalDisk() && destination.IsLocalDisk()) {
        // Both panes are this machine. Rather than a half-supported local copy
        // that would block the UI thread on a large file, say so plainly.
        wxMessageBox("Both panes are showing this computer.\n\n"
                     "Copying between local folders is a job for your file "
                     "manager; this window moves files to and from remote "
                     "sessions.",
                     "Copy", wxOK | wxICON_INFORMATION, this);
        return;
    }

    for (const auto& item : items)
        QueueOne(item, source, destination, destDir);

    destinationDirty_ = true;
    if (status_)
        status_->SetStatusText(
            wxString::Format("Queued %zu item%s for %s",
                             items.size(), items.size() == 1 ? "" : "s",
                             DecodeForDisplay(destination.label)));
}

// ---------------------------------------------------------------------------
// Queue notifications
// ---------------------------------------------------------------------------

void FileExplorerFrame::UpdateQueueStatus()
{
    if (!status_ || !queue_) return;

    const wxString text = queue_->Jobs().empty() ? wxString()
                                                 : DescribeTransferQueue(*queue_);
    // OnTransferJobChanged fires per progress callback; repainting the status
    // bar on every SFTP block would be gratuitous.
    if (text == lastQueueStatus_) return;
    lastQueueStatus_ = text;
    status_->SetStatusText(text, 1);
}

void FileExplorerFrame::OnTransferJobAdded(term::fs::JobId)
{
    if (transfers_) transfers_->RefreshFromQueue();
    UpdateQueueStatus();
}

void FileExplorerFrame::OnTransferJobChanged(term::fs::JobId)
{
    if (transfers_) transfers_->RefreshFromQueue();
    UpdateQueueStatus();
}

void FileExplorerFrame::OnTransferQueueIdle()
{
    if (transfers_) transfers_->RefreshFromQueue();
    UpdateQueueStatus();
    if (!destinationDirty_) return;
    destinationDirty_ = false;

    // Both panes are re-read rather than only the destination: work may have
    // been queued in either direction before this batch drained, and guessing
    // which side changed would sometimes leave a stale listing.
    // A hidden pane costs a round trip nobody sees; ApplyMode refreshes it
    // when it comes back.
    if (leftPane_  && leftPane_->IsLive())  leftPane_->Reload();
    if (rightPane_ && rightPane_->IsShown() && rightPane_->IsLive())
        rightPane_->Reload();

    if (status_) status_->SetStatusText("Transfers finished.");
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

void FileExplorerFrame::OnSessionEnded(term::session::SessionId id)
{
    if (!id) return;

    // Retire only the work that touched this session. A window can now span
    // several, and the rest of the queue is still perfectly valid.
    if (term::transport::IRemoteFileSystem* fs = sm_.GetRemoteFileSystem(id))
        if (queue_) queue_->CancelJobsUsing(fs);

    for (FileExplorerPane* pane : {leftPane_, rightPane_}) {
        if (!pane || pane->CurrentEndpoint().sessionId != id) continue;
        pane->GoOffline(wxString::Format(
            "%s has closed.", DecodeForDisplay(pane->CurrentEndpoint().label)));
    }

    if (transfers_) transfers_->RefreshFromQueue();
    RefreshEndpointChoices();
    UpdateTransferButtons();
}

void FileExplorerFrame::OnDestroy(wxWindowDestroyEvent& evt)
{
    // Destroy events from this frame's own children propagate up to here, so
    // only the frame's own destruction should notify the owner.
    if (evt.GetWindow() != this) { evt.Skip(); return; }

    PersistGeometry();

    // The queue holds callbacks into this frame; retiring it first means
    // nothing can fire into a half-destroyed window.
    if (queue_) {
        queue_->SetListener(nullptr);
        queue_.reset();
    }

    if (onClosed_) {
        auto cb = std::move(onClosed_);
        onClosed_ = nullptr;
        cb();
    }
    evt.Skip();
}

} // namespace ui
