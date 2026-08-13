#include "ui/UIManager.h"
#include "ui/ClipboardUtils.h"
#include "ui/ColorUtils.h"
#include "ui/DialogPlacement.h"
#include "ui/EditorLauncher.h"
#include "ui/ResetAndClearDialog.h"
#include "app/App.h"
#include "ui/FileExplorerManager.h"
#include "ui/RemoteEditManager.h"
#include "ui/RemoteFileBrowserDialog.h"
#include "ui/KbdIntDialog.h"
#include "ui/PasteConfirmDialog.h"
#include "ui/ISessionDropTarget.h"
#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include "ui/TerminalTile.h"
#include "ui/TerminalGrid.h"
#include "layout/DocLayout.h"
#include "ui/SearchBar.h"
#include "ui/SearchController.h"
#include "ui/SelectionActions.h"
#include "ui/StringUtils.h"
#include <wx/brush.h>
#include <wx/utils.h>
#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/string.h>

#include <cstdlib>
#include <future>
#include <string>
#include <unordered_set>
#include <vector>

namespace ui {

// ---------------------------------------------------------------------------
// SessionNotifier — runs on the session thread
// ---------------------------------------------------------------------------

void UIManager::SessionNotifier::OnDocumentChanged(DocChangeType type, size_t)
{
    if (type == DocChangeType::TitleChanged) {
        std::string t = getTitle();
        mgr->frame_->CallAfter([m = mgr, sid = id, title = std::move(t)]() {
            SessionUI* ui = m->FindSessionUI(sid);
            if (!ui) return;
            ui->label = title;
            if (m->onSessionListChanged_) m->onSessionListChanged_();
            if (ui->tile)
                ui->tile->SetTabLabel(sid, wxString::FromUTF8(title));
        });
    } else {
        {
            std::lock_guard<std::mutex> lk(mgr->pendingRefreshMtx_);
            if (!mgr->pendingRefresh_.insert(id).second)
                return;
        }
        mgr->frame_->CallAfter([m = mgr, sid = id]() {
            {
                std::lock_guard<std::mutex> lk(m->pendingRefreshMtx_);
                m->pendingRefresh_.erase(sid);
            }
            SessionUI* ui = m->FindSessionUI(sid);
            if (!ui) return;

            if (ui->tile && ui->tile->GetActiveSessionId() != sid)
                ui->tile->SetTabUnread(sid, true);

            if (ui->panel)
                ui->panel->OnDocumentUpdate();
        });
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

UIManager::UIManager(term::session::SessionManager& sm,
                     MainFrame*                     frame,
                     const AppConfig&               cfg,
                     term::input::InputRouter&      router,
                     wxMenu*                        editMenu)
    : sm_(sm), router_(router), frame_(frame), cfg_(cfg)
{
    selectionActions_ = std::make_unique<SelectionActionRegistry>();
    selectionActions_->Register(std::make_unique<CopyAction>());
    selectionActions_->Register(std::make_unique<SaveToFileAction>());
    selectionActions_->Register(std::make_unique<WebSearchAction>(cfg_.webSearchUrl));
    selectionActions_->Register(std::make_unique<FindInTerminalAction>(
        [this](const std::u32string& q) { ShowSearchBarForActive(true, q); }
    ));
    selectionActions_->Register(std::make_unique<PasteSelectionAction>(
        [this](const std::u32string& text) { DoPaste(ToUtf8(text)); }
    ));

    grid_ = new TerminalGrid(frame_);
    grid_->SetDirection(cfg_.tileLayout);
    frame_->GetSizer()->Add(grid_, 1, wxEXPAND);

    SetupEditMenu(editMenu);

    frame_->Bind(EVT_TERMINAL_ACTION, &UIManager::OnTerminalAction, this);
    frame_->Bind(EVT_TILE_ACTION,     &UIManager::OnTileAction,     this);

    router_.SetOnStateChanged([this] { RefreshBroadcastVisuals(); });
}

void UIManager::UpdateConfig(const AppConfig& cfg)
{
    const bool fontChanged    = cfg.fontFamily != cfg_.fontFamily
                             || cfg.fontSize   != cfg_.fontSize;
    const bool paddingChanged = cfg.padding    != cfg_.padding;

    cfg_ = cfg;
    selectionActions_->UpdateWebSearchUrl(cfg_.webSearchUrl);

    // Propagate to terminal panels and tile chrome.  Deduplicate tile updates
    // by collecting unique tile pointers — multiple sessions share a tile.
    std::unordered_set<TerminalTile*> visitedTiles;
    for (auto& [id, ui] : sessions_) {
        if (ui.panel)
            ui.panel->ApplyConfig(cfg_);
        if (ui.tile && visitedTiles.insert(ui.tile).second)
            ui.tile->ApplyConfig(cfg_);
    }

    if (grid_) grid_->ApplyConfig(cfg_);

    if (fontChanged || paddingChanged)
        RefitAllTiles();
}

void UIManager::SetTileLayout(TileLayout layout)
{
    if (!grid_) return;
    grid_->SetDirection(layout);
    ResizeFrameToFitTiles();
}

TileLayout UIManager::GetTileLayout() const
{
    return grid_ ? grid_->GetDirection() : TileLayout::RowFirst;
}

UIManager::~UIManager()
{
    // Detach all remaining SessionNotifiers from their documents before the
    // sessions are destroyed by SessionManager (which outlives UIManager in
    // WindowContext destruction order).
    for (auto& [id, ui] : sessions_) {
        if (ui.notifier)
            sm_.DetachSessionListener(id, ui.notifier.get());
    }
}

// ---------------------------------------------------------------------------
// ISessionObserver
// ---------------------------------------------------------------------------

void UIManager::OnSessionDisconnected(term::session::SessionId id,
                                       term::transport::DisconnectReason reason)
{
    frame_->CallAfter([this, id, reason]() {
        using R = term::transport::DisconnectReason;

        // Deliberate: Stop() was called — CloseSession already owns the teardown.
        if (reason == R::Deliberate)
            return;

        // Clean exit (e.g. user typed "exit") or non-reconnectable transport:
        // close normally as before.
        if (reason == R::Clean || !IsReconnectable(id)) {
            sm_.CloseSession(id);
            return;
        }

        // Interrupted + reconnectable: preserve session, show indicator.
        // Session::status_ is already Disconnected; the badge updates on next paint.
        SessionUI* ui = FindSessionUI(id);
        if (!ui) return;

        const wxString msg = wxString::FromUTF8("Connection lost.");
        ui->panel->ShowReconnectBar(msg);
    });
}

void UIManager::OnSessionError(term::session::SessionId /*id*/,
                               const term::transport::TransportError& error)
{
    const bool isHostKey =
        (error.category == term::transport::TransportError::Category::HostKey);
    std::string msg = error.message;
    const char* title = isHostKey ? "Host Key Error" : "Connection Error";
    frame_->CallAfter([this, msg, title]() {
        wxMessageBox(wxString::FromUTF8(msg), title, wxICON_ERROR | wxOK, frame_);
    });
}

void UIManager::OnSessionDestroyed(term::session::SessionId id)
{
    // SM-initiated (called after Stop() — transport thread already joined).
    SessionUI* ui = FindSessionUI(id);
    if (ui && ui->notifier) {
        sm_.DetachSessionListener(id, ui->notifier.get());
        ui->notifier.reset();
    }
    if (onSessionDestroyedCb_) onSessionDestroyedCb_(id);
    TearDownSessionUI(id);
}

void UIManager::OnAltScreenChanged(term::session::SessionId id, bool active)
{
    frame_->CallAfter([this, id, active]() {
        SessionUI* sui = FindSessionUI(id);
        if (!sui) return;
        sui->altScrActive = active;
        if (sui->tile && sui->tile->GetActiveSessionId() == id)
            sui->tile->SetAltScrActive(active);
    });
}

void UIManager::OnX11FwdChanged(term::session::SessionId id, bool active)
{
    frame_->CallAfter([this, id, active]() {
        SessionUI* sui = FindSessionUI(id);
        if (!sui) return;
        sui->x11Active = active;
        if (sui->tile && sui->tile->GetActiveSessionId() == id)
            sui->tile->SetX11Active(active);
    });
}

void UIManager::OnBell(term::session::SessionId id)
{
    frame_->CallAfter([this, id]() {
        switch (cfg_.bellMode) {
            case BellMode::Audible:
                wxBell();
                break;
            case BellMode::Visual:
                if (SessionUI* sui = FindSessionUI(id); sui && sui->panel)
                    sui->panel->Flash();
                break;
            case BellMode::None:
                break;
        }
    });
}

void UIManager::OnCursorVisibilityChanged(term::session::SessionId id, bool visible)
{
    frame_->CallAfter([this, id, visible]() {
        SessionUI* sui = FindSessionUI(id);
        if (!sui || !sui->panel) return;
        sui->panel->SetCursorHiddenByApp(!visible);
    });
}

std::vector<std::string> UIManager::OnKbdIntChallenge(
    term::session::SessionId id,
    const term::transport::KbdIntChallenge& challenge)
{
    // Called on the transport worker thread — must dispatch to the UI thread
    // and block until the user responds.
    // shared_ptr makes the promise copyable so the lambda satisfies CallAfter.
    auto promise = std::make_shared<std::promise<std::vector<std::string>>>();
    auto future  = promise->get_future();

    frame_->CallAfter([this, id, challenge, promise]() {
        KbdIntDialog dlg(frame_, challenge);
        // sessions_ is touched only here on the UI thread, not on the caller's
        // transport worker thread.
        if (SessionUI* sui = FindSessionUI(id); sui && sui->tile)
            CentreDialogOnTile(dlg, sui->tile);
        if (dlg.ShowModal() == wxID_OK)
            promise->set_value(dlg.GetResponses());
        else
            promise->set_value({});
    });

    return future.get();
}

// ---------------------------------------------------------------------------
// App-initiated session subscription
// ---------------------------------------------------------------------------

void UIManager::WireTileCallbacks(TerminalTile* tile)
{
    tile->SetDragStartCallback([this](TerminalTile* t, wxPoint pt) {
        OnTileDragStart(t, pt);
    });
    tile->SetTabDragStartCallback([this](term::session::SessionId sid, wxPoint pt) {
        OnTabDragStart(sid, pt);
    });
    tile->SetDropSessionCallback(
        [this](std::span<const term::session::SessionId> ids, TerminalTile* dstTile) -> bool {
            return static_cast<App&>(wxGetApp()).DropSession(ids, frame_, dstTile);
        });
    tile->SetStatusProvider([this](term::session::SessionId id) {
        return sm_.GetSessionStatus(id);
    });
}

void UIManager::TakeSession(term::session::SessionId     id,
                             std::function<std::string()> getTitle,
                             unsigned short               cols,
                             unsigned short               rows,
                             const std::string&           label,
                             TerminalTile*                targetTile)
{
    const bool isNewTile = (targetTile == nullptr);
    if (isNewTile) {
        targetTile = new TerminalTile(grid_, cfg_);
        WireTileCallbacks(targetTile);
    }

    // Create the panel parented to the tile's content area.
    auto* panel = new TerminalPanel(targetTile->GetContentArea(), cfg_, cols, rows);
    panel->SetDocLayout(&sm_.GetDocLayout(id));

    panel->SetScrollCallback([this, id](int topRow) { OnScroll(id, topRow); });
    panel->SetResizeCallback([this, id](unsigned short c, unsigned short r) {
        OnViewportResize(id, c, r);
    });
    panel->SetKeyCallback([this](const term::input::KeyEvent& evt) {
        if (evt.ctrl && evt.key == term::input::Key::Character && evt.code == 'f') {
            ShowSearchBarForActive(true, GetActiveSelectedText());
            return;
        }
        if (evt.ctrl && evt.key == term::input::Key::Character && evt.code == 'v') {
            PasteFromClipboard();
            return;
        }
        router_.Send(evt);
        EnsureCursorVisibleForActive();
    });
    panel->SetPasteCallback([this](const std::string& utf8) { DoPaste(utf8); });
    panel->SetActionRegistry(selectionActions_.get());

    auto ctrl = std::make_unique<SearchController>(sm_.GetDocLayout(id), *panel);
    auto* bar = new SearchBar(panel, *ctrl);
    ctrl->SetBar(bar);
    panel->SetSearchBar(bar);
    panel->SetSearchController(ctrl.get());

    // Wire ReconnectBar buttons. Captured by value so they stay valid after
    // TakeSession returns. The panel is wx-owned and outlives these lambdas.
    panel->SetReconnectCallback([this, id]() {
        SessionUI* ui = FindSessionUI(id);
        if (!ui) return;
        ui->panel->HideReconnectBar();
        sm_.ReconnectSession(id);
    });
    panel->SetReconnectSaveCallback([this, id]() {
        SaveSessionToFile(id);
    });
    panel->SetReconnectCloseCallback([this, id]() {
        sm_.CloseSession(id);
    });

    const std::string resolvedLabel = [&]() -> std::string {
        auto t = getTitle();
        return t.empty() ? label : t;
    }();
    const int tabIdx = targetTile->AddTab(id, panel, wxString::FromUTF8(resolvedLabel));

    // Sync the tile's wrap control to the session's persisted wrap state.
    targetTile->SetWrapMode(sm_.GetDocLayout(id).GetWrapMode());

    // Show the X11 indicator only when the active session is SSH-capable.
    // Query the transport directly — the previous SessionUI (if any) was removed
    // by ReleaseSession before TakeSession is called, so cached state is gone.
    const bool x11Active = sm_.IsX11ForwardingActive(id);

    if (isNewTile) {
        grid_->AddTile(targetTile);
        ResizeFrameToFitTiles();
    }

    auto notifier         = std::make_unique<SessionNotifier>();
    notifier->id          = id;
    notifier->mgr         = this;
    notifier->getTitle    = std::move(getTitle);
    sm_.AttachSessionListener(id, notifier.get());

    SessionUI sui;
    sui.id         = id;
    sui.label      = label;
    sui.tile       = targetTile;
    sui.tabIndex   = tabIdx;
    sui.panel      = panel;
    sui.searchCtrl = std::move(ctrl);
    sui.notifier   = std::move(notifier);
    sui.x11Active  = x11Active;
    sessions_.emplace(id, std::move(sui));

    targetTile->ShowX11Control(sm_.SupportsX11Forwarding(id), x11Active);
    targetTile->SetTabSupportsFileTransfer(id, sm_.SupportsFileTransfer(id));

    const bool supportsPfw = sm_.SupportsPortForwarding(id);
    sessions_.at(id).supportsPortForwarding = supportsPfw;
    if (supportsPfw) {
        sm_.SetPortForwardChangedCallback(id,
            [this, id, tile = targetTile](std::vector<term::transport::PortForwardStatus> status) {
                frame_->CallAfter([this, id, tile, status = std::move(status)]() mutable {
                    SessionUI* sui = FindSessionUI(id);
                    if (!sui) return;
                    auto descs = sm_.GetPortForwardDescs(id);
                    sui->portForwardStatus = status;
                    sui->portForwardDescs  = descs;
                    // Only push to the tile when this session is the active tab.
                    if (tile->GetActiveSessionId() == id)
                        tile->SetPortForwardStatus(std::move(status), std::move(descs));
                });
            });

        // Deliver the initial profile-loaded status into the cache.
        sessions_.at(id).portForwardStatus = sm_.GetPortForwardStatus(id);
        sessions_.at(id).portForwardDescs  = sm_.GetPortForwardDescs(id);
    }

    if (onSessionListChanged_) onSessionListChanged_();
    RequestActivate(id);
    RefreshBroadcastVisuals();
}

void UIManager::ReleaseSession(term::session::SessionId id)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui) return;

    if (ui->notifier) {
        sm_.DetachSessionListener(id, ui->notifier.get());
        ui->notifier.reset();
    }
    TearDownSessionUI(id);
}

bool UIManager::HasSession(term::session::SessionId id) const
{
    return sessions_.count(id) > 0;
}

void UIManager::CloseAllSessions()
{
    std::vector<term::session::SessionId> ids;
    ids.reserve(sessions_.size());
    for (auto& [id, _] : sessions_)
        ids.push_back(id);
    for (auto id : ids)
        sm_.CloseSession(id);
}

void UIManager::ToggleWrapModeForActive()
{
    ToggleWrapModeForSession(activeId_);
}

void UIManager::ToggleWrapModeForSession(term::session::SessionId id)
{
    if (id == 0) return;
    const bool newWrap = !sm_.GetDocLayout(id).GetWrapMode();
    sm_.SetWrapMode(id, newWrap);
    if (auto* sui = FindSessionUI(id); sui && sui->tile)
        sui->tile->SetWrapMode(newWrap);
    if (id == activeId_)
        frame_->SyncwrapModeMenuItem(newWrap);
}

void UIManager::ResetTerminalForSession(term::session::SessionId id)
{
    if (id) sm_.ResetTerminal(id, false);
}

void UIManager::ResetAndClearSession(term::session::SessionId id)
{
    if (!id) return;

    DocLayout& layout = sm_.GetDocLayout(id);
    if (layout.GetLineCount() > 1) {
        ResetAndClearDialog dlg(frame_);
        if (SessionUI* sui = FindSessionUI(id); sui && sui->tile)
            CentreDialogOnTile(dlg, sui->tile);
        const int answer = dlg.ShowModal();

        if (answer == wxID_CANCEL) return;

        if (answer == wxID_YES) {
            layout.SelectAll();
            const auto text = layout.GetSelectedText();
            layout.ClearSelection();
            SaveToFileAction{}.Execute(text);
        }
    }

    sm_.ResetTerminal(id, true);
}

void UIManager::ResetActiveTerminal()         { ResetTerminalForSession(activeId_); }
void UIManager::ResetAndClearActiveTerminal() { ResetAndClearSession(activeId_); }

bool UIManager::ActiveSessionSupportsFileTransfer() const
{
    return activeId_ && sm_.SupportsFileTransfer(activeId_);
}


void UIManager::EditRemoteFileForSession(term::session::SessionId id)
{
    if (!editMgr_ || !id || !sm_.SupportsFileTransfer(id)) return;

    const std::string remote = sm_.GetRemoteDescription(id);
    const std::string cwd    = sm_.GetCurrentWorkingDir(id);
    RemoteFileBrowserDialog dlg(frame_, id, sm_, remote, "Edit", cwd);
    if (SessionUI* sui = FindSessionUI(id); sui && sui->tile)
        CentreDialogOnTile(dlg, sui->tile);
    if (dlg.ShowModal() != wxID_OK) return;

    if (dlg.GetSelectedPath().empty()) return;
    OpenFileInEditor(id, dlg.GetSelectedPath());
}

std::optional<std::string> UIManager::ResolveEditorCommand()
{
    std::string command = cfg_.externalEditorCommand;
    if (command.empty()) {
        if (const char* envEditor = std::getenv("EDITOR")) command = envEditor;
    }
    if (!command.empty()) return command;

    wxMessageBox(
        wxString::FromUTF8(
            "No editor configured.\n\n"
            "Set one in Edit \xe2\x86\x92 Preferences \xe2\x86\x92 Behavior \xe2\x86\x92 External editor."),
        "Open in Editor", wxOK | wxICON_INFORMATION, frame_);
    return std::nullopt;
}

void UIManager::OpenFileInEditor(term::session::SessionId id,
                                 const std::string& path)
{
    const auto command = ResolveEditorCommand();
    if (!command) return;

    // This computer: the editor writes the real file, so there is no copy to
    // download, no watch to run and no upload to schedule.
    if (!id) {
        LaunchEditor(*command, path);
        return;
    }

    if (!editMgr_) return;

    editMgr_->OpenRemoteFile(id, path, *command,
        [this](bool ok, std::string err) {
            if (!ok) {
                wxMessageBox(wxString::FromUTF8("Remote edit failed: " + err),
                             "Edit Remote File", wxOK | wxICON_ERROR, frame_);
            }
        });
}

void UIManager::ReportRemoteSaveFailed(const SaveFailure& failure)
{
    // The editor has already told the user the write succeeded — locally it
    // did. Say plainly that the remote copy is the stale one, and point at the
    // local file, which still holds the edits and is what a retry re-sends.
    const std::string text =
        "Your changes were not saved to the remote file.\n\n"
        + failure.remotePath + "\n"
        + (failure.message.empty() ? "Upload failed." : failure.message) + "\n\n"
        "Your edits are still in the local copy:\n"
        + failure.localPath + "\n\n"
        "Saving again in the editor retries the upload.";

    wxMessageBox(wxString::FromUTF8(text), "Remote Edit",
                 wxOK | wxICON_ERROR, frame_);
}

void UIManager::OpenFileExplorerForSession(term::session::SessionId id,
                                           FileExplorerMode mode)
{
    if (!explorerMgr_ || !id) return;
    explorerMgr_->OpenForSession(frame_, id, mode);
}

void UIManager::OpenFileExplorerForActive(FileExplorerMode mode)
{
    OpenFileExplorerForSession(activeId_, mode);
}

void UIManager::EditRemoteFileForActive()
{
    EditRemoteFileForSession(activeId_);
}

void UIManager::SaveSessionToFile(term::session::SessionId id)
{
    if (!id) return;
    DocLayout& layout = sm_.GetDocLayout(id);
    layout.SelectAll();
    const auto text = layout.GetSelectedText();
    layout.ClearSelection();
    if (!text.empty())
        SaveToFileAction{}.Execute(text);
}

void UIManager::SaveActiveSessionToFile()
{
    SaveSessionToFile(activeId_);
}

void UIManager::ToggleBroadcastMode()
{
    const bool enabling = router_.GetMode() != term::input::InputMode::Broadcast;
    if (enabling) {
        // If no sessions are already selected, seed with just the active session
        // so that enabling broadcast via the menu doesn't silently add every tab.
        bool anySelected = false;
        for (auto& [id, sui] : sessions_) {
            if (auto* t = sm_.GetInputTarget(id); t && router_.IsSelected(t)) {
                anySelected = true;
                break;
            }
        }
        if (!anySelected && activeId_) {
            if (auto* t = sm_.GetInputTarget(activeId_))
                router_.Select(t);
        }
        router_.SetMode(term::input::InputMode::Broadcast);
    } else {
        router_.SetMode(term::input::InputMode::Focused);
    }
    frame_->SyncBroadcastMenuItem(enabling);
}

void UIManager::ToggleTileBroadcast(term::session::SessionId id)
{
    auto* target = sm_.GetInputTarget(id);
    if (!target) return;

    if (router_.IsSelected(target))
        router_.Deselect(target);
    else
        router_.Select(target);

    frame_->SyncBroadcastMenuItem(router_.GetMode() == term::input::InputMode::Broadcast);
}

void UIManager::RefreshBroadcastVisuals()
{
    const bool broadcasting = router_.GetMode() == term::input::InputMode::Broadcast;

    // Accumulate per-tile broadcast state across all sessions.  A tile's title
    // bar shows the broadcast colour as long as ANY of its tabs is in the
    // broadcast group — the user sees the alert even after switching to a
    // non-broadcast tab within that tile.
    std::unordered_map<TerminalTile*, bool> tileHasBroadcast;

    for (auto& [id, sui] : sessions_) {
        if (!sui.tile) continue;
        auto* target = sm_.GetInputTarget(id);
        const bool active = broadcasting && target && router_.IsSelected(target);

        sui.tile->SetTabBroadcast(id, active);
        tileHasBroadcast[sui.tile] = tileHasBroadcast[sui.tile] || active;
    }

    for (auto& [tile, anyBroadcast] : tileHasBroadcast)
        tile->SetBroadcastState(broadcasting, anyBroadcast);
}

// ---------------------------------------------------------------------------
// Common teardown
// ---------------------------------------------------------------------------

void UIManager::TearDownSessionUI(term::session::SessionId id)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui) return;

    if (onSessionListChanged_ && !teardownInProgress_) onSessionListChanged_();

    if (ui->searchCtrl) ui->searchCtrl->SetBar(nullptr);
    if (ui->panel) ui->panel->SetSearchController(nullptr);

    if (ui->tile) {
        const bool tileEmpty = ui->tile->RemoveTab(id);
        if (tileEmpty) {
            grid_->RemoveTile(ui->tile);
            ui->tile->Destroy();
            ResizeFrameToFitTiles();
        }
        ui->tile  = nullptr;
        ui->panel = nullptr;
    }

    sessions_.erase(id);

    if (sessions_.empty() && onGridEmptyCb_)
        onGridEmptyCb_();

    if (activeId_ == id)
        activeId_ = 0;

    if (!sessions_.empty())
        RequestActivate(sessions_.begin()->first);
    else
        frame_->SyncwrapModeMenuItem(false);

    RefreshBroadcastVisuals();
}

// ---------------------------------------------------------------------------
// UI-thread event routing
// ---------------------------------------------------------------------------

void UIManager::RequestActivate(term::session::SessionId id)
{
    sm_.ActivateSession(id, router_);
    activeId_ = id;

    SessionUI* ui = FindSessionUI(id);
    TerminalPanel* activePanel = ui ? ui->panel : nullptr;

    if (ui && ui->tile) {
        grid_->SetActiveTile(ui->tile);
        ui->tile->ActivateTabById(id);  // ensure the correct tab is visible
    }

    RefreshBroadcastVisuals();
    SyncTileHeaderControls(id);

    frame_->Layout();
    if (activePanel)
        activePanel->SetFocus();
}

void UIManager::SyncTileHeaderControls(term::session::SessionId id)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui || !ui->tile) return;

    const bool wrap = sm_.GetDocLayout(id).GetWrapMode();
    ui->tile->SetWrapMode(wrap);
    ui->tile->SetAltScrActive(ui->altScrActive);
    ui->tile->ShowX11Control(sm_.SupportsX11Forwarding(id), ui->x11Active);

    frame_->SyncwrapModeMenuItem(wrap);

    // Rewire port forward panel and icon for the now-active session.
    ui->tile->ShowPortForwardControl(ui->supportsPortForwarding);
    if (ui->supportsPortForwarding) {
        ui->tile->SetPortForwardCallbacks(
            [this, id](term::transport::PortForwardDesc desc) -> term::transport::PortForwardId {
                return sm_.AddPortForward(id, std::move(desc));
            },
            [this, id](term::transport::PortForwardId fwdId) {
                sm_.RemovePortForward(id, fwdId);
            },
            [this, id](term::transport::PortForwardId fwdId) {
                if (savePortForwardToProfileCb_) savePortForwardToProfileCb_(id, fwdId);
            });
        ui->tile->SetPortForwardStatus(ui->portForwardStatus, ui->portForwardDescs);
    } else {
        ui->tile->SetPortForwardStatus({}, {});
    }
}

TerminalTile* UIManager::GetActiveTile() const
{
    const SessionUI* ui = FindSessionUI(activeId_);
    return ui ? ui->tile : nullptr;
}

void UIManager::MoveActiveSessionToNewTile()
{
    const auto id = activeId_;
    if (!id) return;
    frame_->CallAfter([this, id]() {
        static_cast<App&>(wxGetApp()).DropSession({&id, 1}, frame_, nullptr);
    });
}

void UIManager::MoveActiveSessionToNewWindow()
{
    const auto id = activeId_;
    if (!id) return;
    frame_->CallAfter([this, id]() {
        static_cast<App&>(wxGetApp()).DropSession({&id, 1}, nullptr, nullptr);
    });
}

void UIManager::OnScroll(term::session::SessionId id, int topRow)
{
    sm_.GetDocLayout(id).SetTopVisualRow(topRow);
}

void UIManager::OnViewportResize(term::session::SessionId id,
                                  unsigned short cols,
                                  unsigned short rows)
{
    sm_.OnResize(id, cols, rows);
}

void UIManager::SetGeometryForActive(unsigned short cols, unsigned short rows)
{
    SessionUI* ui = FindSessionUI(activeId_);
    if (!ui || !ui->panel) return;

    // Size the tile so that its panel content area fits exactly cols×rows chars.
    // RelayoutTiles() sets every tile to GetMinSize(), so updating the min size
    // here is sufficient — ResizeFrameToFitTiles() triggers the full cascade:
    //   frame OnSize → grid RelayoutTiles → tile OnSize → panel OnSize
    //   → docLayout SetViewportSize + resizeCb (debounced) → Session::SetViewportSize
    UpdateTileMinSize(*ui, cols, rows);
    ResizeFrameToFitTiles();
}

void UIManager::UpdateTileMinSize(SessionUI& ui, unsigned short cols, unsigned short rows)
{
    const wxSize panelSz = ui.panel->ComputeRequiredPanelSize(cols, rows);
    ui.tile->SetMinSize({ panelSz.x, panelSz.y + TerminalTile::kTitleBarHeight });
}

void UIManager::RefitAllTiles()
{
    std::unordered_set<TerminalTile*> updated;
    for (auto& [id, ui] : sessions_) {
        if (!ui.panel || !ui.tile || updated.count(ui.tile)) continue;
        const DocLayout& dl = sm_.GetDocLayout(id);
        UpdateTileMinSize(ui,
            static_cast<unsigned short>(dl.GetViewportCols()),
            static_cast<unsigned short>(dl.GetViewportRows()));
        updated.insert(ui.tile);
    }
    ResizeFrameToFitTiles();
}

std::optional<GeometryPreset> UIManager::GetActiveGeometry() const
{
    const SessionUI* ui = FindSessionUI(activeId_);
    if (!ui || !ui->panel) return std::nullopt;
    const DocLayout& dl = sm_.GetDocLayout(activeId_);
    return GeometryPreset{
        static_cast<unsigned short>(dl.GetViewportCols()),
        static_cast<unsigned short>(dl.GetViewportRows())
    };
}

void UIManager::EnsureCursorVisibleForActive()
{
    SessionUI* ui = FindSessionUI(activeId_);
    if (ui && ui->panel)
        ui->panel->EnsureCursorVisible();
}

void UIManager::OnTerminalAction(TerminalActionEvent& evt)
{
    switch (evt.GetAction())
    {
        case TerminalAction::CloseSession: {
            // Defer: CloseSession → OnSessionDestroyed → tile->Destroy() is
            // synchronous.  Calling it directly here would destroy the TabStrip
            // (and its GTK widget) while still on its OnLeftDown call stack → SEGFAULT.
            const auto id = evt.GetSessionId();
            frame_->CallAfter([this, id]() { sm_.CloseSession(id); });
            break;
        }
        case TerminalAction::ToggleWrap:
            ToggleWrapModeForSession(evt.GetSessionId());
            break;
        case TerminalAction::ToggleBroadcast:
            ToggleTileBroadcast(evt.GetSessionId());
            break;
        case TerminalAction::ToggleAltScr: {
            const auto id = evt.GetSessionId();
            const bool newActive = !sm_.IsAltScreenActive(id);
            sm_.ForceAltScreen(id, newActive);
            if (SessionUI* sui = FindSessionUI(id); sui && sui->tile)
                sui->tile->SetAltScrActive(newActive);
            break;
        }
        case TerminalAction::ToggleX11Fwd:
            // No-op: X11 forwarding is configured at connect time only.
            break;
        case TerminalAction::ResetTerminal:
            ResetTerminalForSession(evt.GetSessionId());
            break;
        case TerminalAction::ResetAndClear:
            ResetAndClearSession(evt.GetSessionId());
            break;
        case TerminalAction::SaveToFile:
            SaveSessionToFile(evt.GetSessionId());
            break;
        case TerminalAction::TransferFiles:
            // The old modal dialog's entry point now opens the explorer
            // already in the shape that job needs.
            OpenFileExplorerForSession(evt.GetSessionId(),
                                       FileExplorerMode::Transfer);
            break;
        case TerminalAction::EditRemoteFile:
            EditRemoteFileForSession(evt.GetSessionId());
            break;
    }
}

void UIManager::OnTileAction(TileActionEvent& evt)
{
    switch (evt.GetAction())
    {
        case TileAction::CloseTab: {
            const auto id = evt.GetSessionId();
            frame_->CallAfter([this, id]() { sm_.CloseSession(id); });
            break;
        }
        case TileAction::NewTabHere:
            OnNewTabRequest(evt.GetTile());
            break;
        case TileAction::ActivateSession:
            RequestActivate(evt.GetSessionId());
            break;
        case TileAction::MoveToNewTile: {
            const auto id = evt.GetSessionId();
            frame_->CallAfter([this, id]() {
                static_cast<App&>(wxGetApp()).DropSession({&id, 1}, frame_, nullptr);
            });
            break;
        }
        case TileAction::MoveToNewWindow: {
            const auto id = evt.GetSessionId();
            frame_->CallAfter([this, id]() {
                static_cast<App&>(wxGetApp()).DropSession({&id, 1}, nullptr, nullptr);
            });
            break;
        }
        case TileAction::OpenFileExplorer:
            OpenFileExplorerForSession(evt.GetSessionId(),
                                       FileExplorerMode::Explore);
            break;
        case TileAction::MoveAllToNewWindow: {
            TerminalTile* tile = evt.GetTile();
            std::vector<term::session::SessionId> ids;
            ids.reserve(tile->GetTabCount());
            for (int i = 0; i < tile->GetTabCount(); ++i)
                ids.push_back(tile->GetSessionIdByTabIndex(i));
            frame_->CallAfter([this, ids = std::move(ids)]() {
                static_cast<App&>(wxGetApp()).DropSession(ids, nullptr, nullptr);
            });
            break;
        }
    }
}

void UIManager::OnNewTabRequest(TerminalTile* tile)
{
    // Delegate to MainFrame which owns the connection dialog.
    frame_->LaunchNewConnectionInTile(tile);
}

// ---------------------------------------------------------------------------
// Drag support
// ---------------------------------------------------------------------------

void UIManager::BeginDragGesture(const wxString& label, wxPoint screenAnchor)
{
    const auto& u = cfg_.uiColors;
    dragGhost_ = std::make_unique<DragGhost>(frame_, label, toWx(u.tileInactive), toWx(u.tabText));
    dragGhost_->MoveTo(screenAnchor);
    dragGhost_->Show();
    frame_->CaptureMouse();
    frame_->Bind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);
    frame_->Bind(wxEVT_MOTION,  &UIManager::OnDragMotion,  this);
}

void UIManager::OnTileDragStart(TerminalTile* tile, wxPoint screenAnchor)
{
    if (!tile || dragState_) return;

    DragState state;
    state.ids.reserve(tile->GetTabCount());
    for (int i = 0; i < tile->GetTabCount(); ++i) {
        auto sid = tile->GetSessionIdByTabIndex(i);
        if (sid != 0) state.ids.push_back(sid);
    }
    if (state.ids.empty()) return;

    state.intent  = DragIntent::Tile;
    state.srcTile = tile;
    dragState_ = std::move(state);

    wxString label = wxString::FromUTF8(sm_.GetLabel(tile->GetActiveSessionId()));
    if (dragState_->ids.size() > 1)
        label += wxString::Format(" (+%d)", (int)dragState_->ids.size() - 1);
    BeginDragGesture(label, screenAnchor);
}

void UIManager::OnTabDragStart(term::session::SessionId id, wxPoint screenAnchor)
{
    if (!FindSessionUI(id) || dragState_) return;

    dragState_ = DragState{{ id }};

    BeginDragGesture(wxString::FromUTF8(sm_.GetLabel(id)), screenAnchor);
}

void UIManager::OnDragMotion(wxMouseEvent& evt)
{
    if (dragGhost_)
        dragGhost_->MoveTo(frame_->ClientToScreen(evt.GetPosition()));
    evt.Skip();
}

void UIManager::OnDragRelease(wxMouseEvent& evt)
{
    if (frame_->HasCapture()) frame_->ReleaseMouse();
    wxSetCursor(wxNullCursor);

    dragGhost_.reset();

    // Defer Unbind — calling it here modifies the dynamic event table while
    // wxEvtHandler::SearchDynamicEventTable is still iterating it, triggering
    // a wx assertion.  CallAfter defers to the next event loop iteration.
    frame_->CallAfter([this]() {
        frame_->Unbind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);
        frame_->Unbind(wxEVT_MOTION,  &UIManager::OnDragMotion,  this);
    });

    if (!dragState_) { evt.Skip(); return; }
    auto state = std::move(*dragState_);
    dragState_.reset();

    const wxPoint screenPt = frame_->ClientToScreen(evt.GetPosition());
    wxWindow* hit = wxFindWindowAtPoint(screenPt);

    ui::ISessionDropTarget* target = nullptr;
    for (wxWindow* w = hit; w; w = w->GetParent()) {
        if (auto* t = dynamic_cast<ui::ISessionDropTarget*>(w)) {
            target = t;
            break;
        }
    }

    if (!target) { evt.Skip(); return; }

    if (state.intent == DragIntent::Tile) {
        auto* dstTile = dynamic_cast<TerminalTile*>(target);
        bool sameFrame = false;
        for (wxWindow* w = dynamic_cast<wxWindow*>(target); w; w = w->GetParent())
            if (w == frame_) { sameFrame = true; break; }

        if (dstTile && sameFrame && state.srcTile != dstTile) {
            grid_->MoveTileNear(state.srcTile, dstTile, screenPt);
            evt.Skip(); return;
        }
    }

    // App::DropSession now owns the full transfer atomically (release-then-take),
    // so no post-hoc ReleaseSession is needed here.
    target->DropSession(state.ids, state.intent, screenPt);
    evt.Skip();
}

// ---------------------------------------------------------------------------
// Search / selection helpers
// ---------------------------------------------------------------------------

SearchController* UIManager::GetActiveSearchController()
{
    SessionUI* ui = FindSessionUI(activeId_);
    return ui ? ui->searchCtrl.get() : nullptr;
}

bool UIManager::SearchBarHasFocus() const
{
    const SessionUI* ui = FindSessionUI(activeId_);
    if (!ui || !ui->panel) return false;
    return ui->panel->HasSearchBarFocus();
}

void UIManager::ShowSearchBarForActive(bool show, const std::u32string& initialQuery)
{
    SessionUI* ui = FindSessionUI(activeId_);
    if (!ui || !ui->panel) return;
    ui->panel->ShowSearchBar(show);
    if (show && !initialQuery.empty() && ui->searchCtrl)
        ui->searchCtrl->SetInitialQuery(initialQuery);
}

std::u32string UIManager::GetActiveSelectedText() const
{
    const SessionUI* ui = FindSessionUI(activeId_);
    if (!ui) return {};
    const std::u32string full = sm_.GetDocLayout(ui->id).GetSelectedText();
    const auto nl = full.find(U'\n');
    return (nl != std::u32string::npos) ? full.substr(0, nl) : full;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

UIManager::SessionUI* UIManager::FindSessionUI(term::session::SessionId id)
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? &it->second : nullptr;
}

const UIManager::SessionUI* UIManager::FindSessionUI(term::session::SessionId id) const
{
    auto it = sessions_.find(id);
    return (it != sessions_.end()) ? &it->second : nullptr;
}

bool UIManager::IsReconnectable(term::session::SessionId id) const
{
    const term::session::Connection conn = sm_.GetConnection(id);
    return std::holds_alternative<term::transport::SshDesc>(conn.transport)
        || std::holds_alternative<term::transport::SerialDesc>(conn.transport);
}

std::vector<std::pair<term::session::SessionId, std::string>>
UIManager::GetSessionList() const
{
    std::vector<std::pair<term::session::SessionId, std::string>> out;
    out.reserve(sessions_.size());
    for (const auto& [id, sui] : sessions_)
        out.emplace_back(id, sui.label);
    return out;
}

std::vector<ui::UIManager::TileSnapshot> UIManager::GetTileSnapshots() const
{
    std::vector<TileSnapshot> result;
    if (!grid_)
        return result;
    for (TerminalTile* tile : grid_->GetTiles()) {
        TileSnapshot snap;
        snap.tabOrder      = tile->GetTabOrder();
        snap.activeTabIndex = tile->GetActiveTabIndex();
        if (!snap.tabOrder.empty())
            result.push_back(std::move(snap));
    }
    return result;
}

void UIManager::SetupEditMenu(wxMenu* menu)
{
    menu->Append(kEditMenuCopy,            "Copy\tCtrl+Shift+C");
    menu->Append(kEditMenuPaste,           "Paste\tCtrl+Shift+V");
    menu->Append(kEditMenuPasteSel,        "Paste Selection");
    menu->AppendSeparator();
    menu->Append(kEditMenuFind,            "Find in Terminal\tCtrl+Shift+F");
    menu->Append(kEditMenuSelectAll,       "Select All\tCtrl+Shift+A");
    menu->AppendSeparator();
    menu->Append(kEditMenuSaveFile,        "Save Selection to File...");
    menu->Append(kEditMenuSaveSessionFile, "Save Session to File...");
    menu->Append(kEditMenuWebSearch,       "Search the Web");

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        const auto text = GetFullActiveSelectedText();
        if (!text.empty()) CopyAction{}.Execute(text);
    }, kEditMenuCopy);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        PasteFromClipboard();
    }, kEditMenuPaste);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        SessionUI* ui = FindSessionUI(activeId_);
        if (!ui) return;
        sm_.GetDocLayout(ui->id).SelectAll();
        if (ui->panel) ui->panel->Refresh();
    }, kEditMenuSelectAll);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        DoPaste(ToUtf8(GetFullActiveSelectedText()));
    }, kEditMenuPasteSel);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        ShowSearchBarForActive(true, GetActiveSelectedText());
    }, kEditMenuFind);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        const auto text = GetFullActiveSelectedText();
        if (!text.empty()) SaveToFileAction{}.Execute(text);
    }, kEditMenuSaveFile);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        const auto text = GetFullActiveSelectedText();
        if (!text.empty()) WebSearchAction(cfg_.webSearchUrl).Execute(text);
    }, kEditMenuWebSearch);

    frame_->Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        SaveActiveSessionToFile();
    }, kEditMenuSaveSessionFile);

    const auto enableIfSelection = [this](wxUpdateUIEvent& e) {
        e.Enable(HasActiveSelection());
    };
    for (int id : {kEditMenuCopy, kEditMenuPasteSel,
                   kEditMenuSaveFile, kEditMenuWebSearch}) {
        frame_->Bind(wxEVT_UPDATE_UI, enableIfSelection, id);
    }

    frame_->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) {
        e.Enable(activeId_ != 0);
    }, kEditMenuSaveSessionFile);
}

void UIManager::ResizeFrameToFitTiles()
{
    const int displayIdx = wxDisplay::GetFromWindow(frame_);
    const wxDisplay display(displayIdx == wxNOT_FOUND ? 0 : displayIdx);
    const wxRect workArea = display.GetClientArea();

    const wxSize chrome = frame_->GetSize() - frame_->GetClientSize();
    const int maxClientW = workArea.GetWidth()  - chrome.x;
    const int maxClientH = workArea.GetHeight() - chrome.y;

    // For ColumnFirst, pass maxClientH so the simulation inside
    // ComputeIdealGridSize uses the same wrap constraint as RelayoutTiles will.
    const wxSize ideal = grid_->ComputeIdealGridSize(maxClientH);
    if (ideal.x <= 0 || ideal.y <= 0) return;

    frame_->SetClientSize(std::min(ideal.x, maxClientW),
                          std::min(ideal.y, maxClientH));
}

void UIManager::PasteFromClipboard()
{
    DoPaste(ReadClipboardText());
}

void UIManager::DoPaste(const std::string& utf8)
{
    if (utf8.empty()) return;

    const bool hasNewline = utf8.find('\n') != std::string::npos
                         || utf8.find('\r') != std::string::npos;
    if (hasNewline && !sm_.IsBracketedPasteActive(activeId_)) {
        PasteConfirmDialog dlg(frame_, utf8);
        if (SessionUI* sui = FindSessionUI(activeId_); sui && sui->tile)
            CentreDialogOnTile(dlg, sui->tile);
        if (dlg.ShowModal() != wxID_OK)
            return;
    }

    router_.Paste(utf8);
    EnsureCursorVisibleForActive();
}

bool UIManager::HasActiveSelection() const
{
    const SessionUI* ui = FindSessionUI(activeId_);
    return ui && sm_.GetDocLayout(ui->id).HasSelection();
}

std::u32string UIManager::GetFullActiveSelectedText() const
{
    const SessionUI* ui = FindSessionUI(activeId_);
    return ui ? sm_.GetDocLayout(ui->id).GetSelectedText() : std::u32string{};
}

} // namespace ui
