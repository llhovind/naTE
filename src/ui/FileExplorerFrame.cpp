#include "ui/FileExplorerFrame.h"

#include "fs/RemotePath.h"
#include "ui/ColorUtils.h"
#include "ui/ConflictDialog.h"
#include "ui/StringUtils.h"
#include "ui/TransferPanel.h"

#include <filesystem>

#include <wx/app.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>

namespace ui {

namespace {

constexpr int kInitialWidth   = 1180;
constexpr int kInitialHeight  = 700;
constexpr int kMinWidth       = 620;
constexpr int kMinHeight      = 420;
constexpr int kSplitterMinPane = 280;

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
{
    status_ = CreateStatusBar();
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
}

// ---------------------------------------------------------------------------
// Endpoints
// ---------------------------------------------------------------------------

PaneEndpoint FileExplorerFrame::LocalEndpoint() const
{
    return {"This computer", &SharedLocalFileSystem(), 0, DefaultLocalPath()};
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

void FileExplorerFrame::BuildLayout(std::function<void(std::string)> onOpenInEditor)
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
    outer->Add(splitter_, 1, wxEXPAND | wxALL, 4);

    queue_ = std::make_unique<term::fs::TransferQueue>(
        [](std::function<void()> fn) { wxTheApp->CallAfter(std::move(fn)); });
    queue_->SetListener(this);
    queue_->SetConflictPrompt([this](const term::fs::TransferJob& job, auto respond) {
        ConflictDialog dlg(this, cfg_, job);
        dlg.ShowModal();
        respond(dlg.Resolution(), dlg.ApplyToAll());
    });

    auto* controls = new wxBoxSizer(wxHORIZONTAL);
    splitBtn_   = new wxButton(this, wxID_ANY, "Show Second Pane");
    toRightBtn_ = new wxButton(this, wxID_ANY, "Copy  >");
    toLeftBtn_  = new wxButton(this, wxID_ANY, "<  Copy");

    controls->Add(splitBtn_, 0, wxRIGHT, 16);
    controls->AddStretchSpacer();
    controls->Add(toRightBtn_, 0, wxRIGHT, 8);
    controls->Add(toLeftBtn_,  0);
    controls->AddStretchSpacer();
    outer->Add(controls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    transfers_ = new TransferPanel(this, cfg_, *queue_);
    transfers_->SetOnCancelJob([this](term::fs::JobId id) { queue_->CancelJob(id); });
    transfers_->SetOnCancelAll([this] { queue_->CancelAll(); });
    transfers_->SetOnClearFinished([this] {
        queue_->ClearFinished();
        transfers_->RefreshFromQueue();
    });
    outer->Add(transfers_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

    splitBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        ShowSecondPane(!secondPaneShown_);
    });
    toRightBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        CopyBetweenPanes(leftPane_, rightPane_);
    });
    toLeftBtn_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        CopyBetweenPanes(rightPane_, leftPane_);
    });

    const auto onState = [this] { UpdateTransferButtons(); };
    leftPane_->SetOnStateChanged(onState);
    rightPane_->SetOnStateChanged(onState);
    leftPane_->SetOnEndpointChanged(onState);
    rightPane_->SetOnEndpointChanged(onState);

    SetSizer(outer);
}

void FileExplorerFrame::ShowSecondPane(bool show)
{
    if (show == secondPaneShown_) return;
    secondPaneShown_ = show;

    if (show) {
        rightPane_->Show();
        splitter_->SplitVertically(leftPane_, rightPane_, GetClientSize().x / 2);
        // The list may have gone stale while hidden, and the endpoints
        // certainly may have.
        rightPane_->RefreshEndpointChoices();
        rightPane_->Refresh();
    } else {
        splitter_->Unsplit(rightPane_);
    }

    splitBtn_->SetLabel(show ? "Hide Second Pane" : "Show Second Pane");
    UpdateTransferButtons();
    Layout();
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
    for (wxButton* b : {toRightBtn_, toLeftBtn_, splitBtn_}) {
        if (!b) continue;
        b->SetBackgroundColour(btnBg);
        b->SetForegroundColour(btnFg);
    }
    wxFrame::Refresh();
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

void FileExplorerFrame::UpdateTransferButtons()
{
    const bool paired = secondPaneShown_ && leftPane_ && rightPane_ &&
                        leftPane_->IsLive() && rightPane_->IsLive();

    if (!paired) {
        if (toRightBtn_) { toRightBtn_->Enable(false); toRightBtn_->SetLabel("Copy  >"); }
        if (toLeftBtn_)  { toLeftBtn_->Enable(false);  toLeftBtn_->SetLabel("<  Copy"); }
        return;
    }

    // Label the buttons with where the bytes would actually land, so the
    // direction is unambiguous once a pane can point anywhere.
    toRightBtn_->SetLabel(
        wxString::Format("Copy to %s  >",
                         DecodeForDisplay(rightPane_->CurrentEndpoint().label)));
    toLeftBtn_->SetLabel(
        wxString::Format("<  Copy to %s",
                         DecodeForDisplay(leftPane_->CurrentEndpoint().label)));

    toRightBtn_->Enable(!leftPane_->SelectedItems().empty());
    toLeftBtn_->Enable(!rightPane_->SelectedItems().empty());
    Layout();
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

void FileExplorerFrame::OnTransferJobAdded(term::fs::JobId)
{
    if (transfers_) transfers_->RefreshFromQueue();
}

void FileExplorerFrame::OnTransferJobChanged(term::fs::JobId)
{
    if (transfers_) transfers_->RefreshFromQueue();
}

void FileExplorerFrame::OnTransferQueueIdle()
{
    if (transfers_) transfers_->RefreshFromQueue();
    if (!destinationDirty_) return;
    destinationDirty_ = false;

    // Both panes are re-read rather than only the destination: work may have
    // been queued in either direction before this batch drained, and guessing
    // which side changed would sometimes leave a stale listing.
    if (leftPane_  && leftPane_->IsLive())  leftPane_->Refresh();
    if (rightPane_ && rightPane_->IsLive()) rightPane_->Refresh();

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
