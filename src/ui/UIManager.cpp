#include "ui/UIManager.h"
#include "app/App.h"
#include "ui/FileTransferDialog.h"
#include "ui/ISessionDropTarget.h"
#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include "ui/TerminalTile.h"
#include "ui/TerminalGrid.h"
#include "ui/DocLayout.h"
#include "ui/SearchBar.h"
#include "ui/SearchController.h"
#include "ui/SelectionActions.h"
#include "ui/StringUtils.h"
#include <wx/brush.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/display.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/string.h>

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
            if (ui && ui->panel)
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
    selectionActions_->Register(std::make_unique<WebSearchAction>());
    selectionActions_->Register(std::make_unique<FindInTerminalAction>(
        [this](const std::u32string& q) { ShowSearchBarForActive(true, q); }
    ));
    selectionActions_->Register(std::make_unique<PasteSelectionAction>(
        [this](const std::u32string& text) { router_.Paste(ToUtf8(text)); }
    ));

    grid_ = new TerminalGrid(frame_);
    frame_->GetSizer()->Add(grid_, 1, wxEXPAND);

    SetupEditMenu(editMenu);

    frame_->Bind(EVT_TERMINAL_ACTION, &UIManager::OnTerminalAction, this);
    frame_->Bind(EVT_TILE_ACTION,     &UIManager::OnTileAction,     this);

    router_.SetOnStateChanged([this] { RefreshBroadcastVisuals(); });
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

void UIManager::OnSessionDisconnected(term::session::SessionId id)
{
    frame_->CallAfter([this, id]() {
        sm_.CloseSession(id);
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
    TearDownSessionUI(id);
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
            wxString text;
            if (wxTheClipboard->Open()) {
                if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
                    wxTextDataObject data;
                    wxTheClipboard->GetData(data);
                    text = data.GetText();
                }
                wxTheClipboard->Close();
            }
            if (!text.empty()) {
                router_.Paste(text.ToStdString());
                EnsureCursorVisibleForActive();
            }
            return;
        }
        router_.Send(evt);
        EnsureCursorVisibleForActive();
    });
    panel->SetActionRegistry(selectionActions_.get());

    auto ctrl = std::make_unique<SearchController>(sm_.GetDocLayout(id), *panel);
    auto* bar = new SearchBar(panel, *ctrl);
    ctrl->SetBar(bar);
    panel->SetSearchBar(bar);

    const int tabIdx = targetTile->AddTab(id, panel, wxString::FromUTF8(label));

    // Sync the tile's wrap control to the session's persisted wrap state.
    targetTile->SetWrapMode(sm_.GetDocLayout(id).GetWrapMode());

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
    sessions_.emplace(id, std::move(sui));

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

void UIManager::ResetActiveTerminal()
{
    if (activeId_) sm_.ResetTerminal(activeId_, false);
}

void UIManager::ResetAndClearActiveTerminal()
{
    if (!activeId_) return;

    DocLayout& layout = sm_.GetDocLayout(activeId_);
    if (layout.GetLineCount() > 1) {
        const int answer = wxMessageBox(
            "Save scrollback before clearing?",
            "Reset and Clear",
            wxYES_NO | wxCANCEL | wxICON_QUESTION);

        if (answer == wxCANCEL) return;

        if (answer == wxYES) {
            layout.SelectAll();
            const auto text = layout.GetSelectedText();
            layout.ClearSelection();
            SaveToFileAction{}.Execute(text);
        }
    }

    sm_.ResetTerminal(activeId_, true);
}

void UIManager::SendFilesForActive()
{
    if (!activeId_ || !sm_.SupportsFileTransfer(activeId_)) return;
    const std::string remote = sm_.GetRemoteDescription(activeId_);
    ui::FileTransferDialog dlg(frame_, activeId_, sm_, remote, ui::TransferDirection::Send);
    dlg.ShowModal();
}

void UIManager::ReceiveFilesForActive()
{
    if (!activeId_ || !sm_.SupportsFileTransfer(activeId_)) return;
    const std::string remote = sm_.GetRemoteDescription(activeId_);
    ui::FileTransferDialog dlg(frame_, activeId_, sm_, remote, ui::TransferDirection::Receive);
    dlg.ShowModal();
}

void UIManager::SaveActiveSessionToFile()
{
    if (!activeId_) return;

    DocLayout& layout = sm_.GetDocLayout(activeId_);
    layout.SelectAll();
    const auto text = layout.GetSelectedText();
    layout.ClearSelection();

    if (!text.empty())
        SaveToFileAction{}.Execute(text);
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

    if (onSessionListChanged_) onSessionListChanged_();

    if (ui->searchCtrl) ui->searchCtrl->SetBar(nullptr);

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

    frame_->Layout();
    frame_->SyncwrapModeMenuItem(sm_.GetDocLayout(id).GetWrapMode());
    if (activePanel)
        activePanel->SetFocus();
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
    const wxSize panelSz = ui->panel->ComputeRequiredPanelSize(cols, rows);
    ui->tile->SetMinSize({ panelSz.x, panelSz.y + TerminalTile::kTitleBarHeight });
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

void UIManager::OnTileDragStart(TerminalTile* tile, wxPoint /*screenAnchor*/)
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
    frame_->CaptureMouse();
    frame_->Bind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);
}

void UIManager::OnTabDragStart(term::session::SessionId id, wxPoint /*screenAnchor*/)
{
    if (!FindSessionUI(id) || dragState_) return;

    dragState_ = DragState{{ id }};
    frame_->CaptureMouse();
    frame_->Bind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);
}

void UIManager::OnDragRelease(wxMouseEvent& evt)
{
    if (frame_->HasCapture()) frame_->ReleaseMouse();
    wxSetCursor(wxNullCursor);

    // Defer Unbind — calling it here modifies the dynamic event table while
    // wxEvtHandler::SearchDynamicEventTable is still iterating it, triggering
    // a wx assertion.  CallAfter defers to the next event loop iteration.
    frame_->CallAfter([this]() {
        frame_->Unbind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);
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

std::vector<std::pair<term::session::SessionId, std::string>>
UIManager::GetSessionList() const
{
    std::vector<std::pair<term::session::SessionId, std::string>> out;
    out.reserve(sessions_.size());
    for (const auto& [id, sui] : sessions_)
        out.emplace_back(id, sui.label);
    return out;
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
    menu->Append(kEditMenuSaveFile,        "Save to File...");
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
        const auto text = GetFullActiveSelectedText();
        if (!text.empty()) router_.Paste(ToUtf8(text));
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
        if (!text.empty()) WebSearchAction{}.Execute(text);
    }, kEditMenuWebSearch);

    frame_->Bind(wxEVT_MENU, [](wxCommandEvent&) {
        wxMessageBox("Not yet implemented", "naTE", wxOK | wxICON_INFORMATION);
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
    const wxSize ideal = grid_->ComputeIdealGridSize();
    if (ideal.x <= 0 || ideal.y <= 0) return;

    const int displayIdx = wxDisplay::GetFromWindow(frame_);
    const wxDisplay display(displayIdx == wxNOT_FOUND ? 0 : displayIdx);
    const wxRect workArea = display.GetClientArea();

    const wxSize chrome = frame_->GetSize() - frame_->GetClientSize();
    const int maxClientW = workArea.GetWidth()  - chrome.x;
    const int maxClientH = workArea.GetHeight() - chrome.y;

    frame_->SetClientSize(std::min(ideal.x, maxClientW),
                          std::min(ideal.y, maxClientH));
}

void UIManager::PasteFromClipboard()
{
    wxString text;
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            text = data.GetText();
        }
        wxTheClipboard->Close();
    }
    if (!text.empty()) {
        router_.Paste(text.ToStdString());
        EnsureCursorVisibleForActive();
    }
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
