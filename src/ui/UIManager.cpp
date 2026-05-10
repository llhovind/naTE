#include "ui/UIManager.h"
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
#include <wx/dcmemory.h>
#include <wx/generic/dragimgg.h>
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
            m->connMenu_->SetLabel(ui->menuId, wxString::FromUTF8(title));
            if (ui->tile)
                ui->tile->SetTileLabel(wxString::FromUTF8(title));
            m->UpdateStatusBar();
            if (sid == m->activeId_)
                m->frame_->SetTitle(wxString::FromUTF8("naTE \xe2\x80\x94 " + title));
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
            m->UpdateStatusBar();
        });
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

UIManager::UIManager(term::session::SessionManager& sm,
                     wxMenu*                        connMenu,
                     MainFrame*                     frame,
                     const AppConfig&               cfg,
                     term::input::InputRouter&      router,
                     wxMenu*                        editMenu)
    : sm_(sm), router_(router), connMenu_(connMenu), frame_(frame), cfg_(cfg)
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
// ISessionObserver — 3-method slim interface
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
    frame_->CallAfter([this, msg, isHostKey]() {
        if (isHostKey) {
            wxMessageBox(wxString::FromUTF8(msg), "Host Key Error",
                         wxICON_ERROR | wxOK, frame_);
        } else {
            pendingErrorMsg_ = msg;
        }
    });
}

void UIManager::OnSessionDestroyed(term::session::SessionId id)
{
    // SM-initiated (called after Stop() — transport thread already joined).
    // Detach notifier first (safe without Document mutex since thread is stopped).
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

void UIManager::TakeSession(term::session::SessionId  id,
                             std::function<std::string()> getTitle,
                             unsigned short            cols,
                             const std::string&        label)
{
    auto* tile  = new TerminalTile(grid_, cfg_, cols, label);
    auto* panel = tile->GetTerminalPanel();

    panel->SetDocLayout(&sm_.GetDocLayout(id));

    panel->SetScrollCallback([this, id](int topRow) { OnScroll(id, topRow); });
    panel->SetResizeCallback([this, id](unsigned short c, unsigned short r) {
        OnViewportResize(id, c, r);
    });
    panel->SetFocusCallback([this, id]() { RequestActivate(id); });
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

    tile->SetActivateCallback([this, id]() { RequestActivate(id); });
    tile->SetBroadcastToggleCallback([this, id]() { ToggleTileBroadcast(id); });
    tile->SetTileSessionId(id);
    tile->SetDragStartCallback([this](term::session::SessionId sid, wxPoint pt) {
        OnTileDragStart(sid, pt);
    });

    auto ctrl = std::make_unique<SearchController>(sm_.GetDocLayout(id), *panel);
    auto* bar = new SearchBar(panel, *ctrl);
    ctrl->SetBar(bar);
    panel->SetSearchBar(bar);

    grid_->AddTile(tile);
    ResizeFrameToFitTiles();

    const int menuId = nextMenuId_++;
    connMenu_->Append(menuId, wxString::FromUTF8(label));
    frame_->Bind(wxEVT_MENU, [this, id](wxCommandEvent&) {
        RequestActivate(id);
    }, menuId);

    // Create SessionNotifier and attach it to the session's document.
    auto notifier         = std::make_unique<SessionNotifier>();
    notifier->id          = id;
    notifier->mgr         = this;
    notifier->getTitle    = std::move(getTitle);
    sm_.AttachSessionListener(id, notifier.get());

    SessionUI sui;
    sui.id         = id;
    sui.label      = label;
    sui.menuId     = menuId;
    sui.tile       = tile;
    sui.panel      = panel;
    sui.searchCtrl = std::move(ctrl);
    sui.notifier   = std::move(notifier);
    sessions_.emplace(id, std::move(sui));

    RequestActivate(id);
    RefreshBroadcastVisuals();
}

void UIManager::ReleaseSession(term::session::SessionId id)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui) return;

    // Detach notifier with transport still running — Document mutex makes this safe.
    if (ui->notifier) {
        sm_.DetachSessionListener(id, ui->notifier.get());
        ui->notifier.reset();
    }
    TearDownSessionUI(id);
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

void UIManager::ToggleWordWrapForActive()
{
    if (activeId_ == 0) return;
    const bool newWrap = !sm_.GetDocLayout(activeId_).GetWordWrap();
    sm_.SetWordWrap(activeId_, newWrap);
    frame_->SyncWordWrapMenuItem(newWrap);
}

void UIManager::ToggleBroadcastMode()
{
    const bool enabling = router_.GetMode() != term::input::InputMode::Broadcast;
    if (enabling) {
        bool anySelected = false;
        for (auto& [id, sui] : sessions_) {
            if (auto* t = sm_.GetInputTarget(id); t && router_.IsSelected(t)) {
                anySelected = true;
                break;
            }
        }
        if (!anySelected) {
            for (auto& [id, sui] : sessions_) {
                if (auto* t = sm_.GetInputTarget(id))
                    router_.Select(t);
            }
        }
        router_.SetMode(term::input::InputMode::Broadcast);
    } else {
        router_.SetMode(term::input::InputMode::Focused);
    }
    RefreshBroadcastVisuals();
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
    RefreshBroadcastVisuals();
}

void UIManager::RefreshBroadcastVisuals()
{
    const bool broadcasting = router_.GetMode() == term::input::InputMode::Broadcast;
    for (auto& [id, sui] : sessions_) {
        if (!sui.tile) continue;
        auto* target = sm_.GetInputTarget(id);
        const bool active = broadcasting && target && router_.IsSelected(target);
        sui.tile->SetBroadcastActive(active);
    }
}

// ---------------------------------------------------------------------------
// Common teardown (called by both OnSessionDestroyed and ReleaseSession)
// ---------------------------------------------------------------------------

void UIManager::TearDownSessionUI(term::session::SessionId id)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui) return;

    connMenu_->Delete(ui->menuId);

    if (ui->searchCtrl) ui->searchCtrl->SetBar(nullptr);

    if (ui->tile) {
        grid_->RemoveTile(ui->tile);
        ui->tile->Destroy();
        ui->tile  = nullptr;
        ui->panel = nullptr;
        ResizeFrameToFitTiles();
    }

    sessions_.erase(id);

    if (activeId_ == id)
        activeId_ = 0;

    if (!sessions_.empty())
        RequestActivate(sessions_.begin()->first);
    else {
        UpdateStatusBar();
        frame_->SetTitle("naTE");
    }

    if (!pendingErrorMsg_.empty()) {
        frame_->SetStatusText(wxString::FromUTF8(pendingErrorMsg_), 1);
        pendingErrorMsg_.clear();
    }

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

    if (ui && ui->tile)
        grid_->SetActiveTile(ui->tile);

    frame_->Layout();
    UpdateStatusBar();
    if (ui)
        frame_->SetTitle(wxString::FromUTF8("naTE \xe2\x80\x94 " + ui->label));
    if (activePanel)
        activePanel->SetFocus();
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

void UIManager::EnsureCursorVisibleForActive()
{
    SessionUI* ui = FindSessionUI(activeId_);
    if (ui && ui->panel)
        ui->panel->EnsureCursorVisible();
}

// ---------------------------------------------------------------------------
// Drag support
// ---------------------------------------------------------------------------

void UIManager::OnTileDragStart(term::session::SessionId id, wxPoint screenAnchor)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui) return;

    // Create a label-text bitmap as the drag visual.
    const wxString label = wxString::FromUTF8(ui->label);
    const int bmpW = frame_->GetTextExtent(label).x + 16;
    const int bmpH = TerminalTile::kTitleBarHeight;
    wxBitmap bmp(bmpW, bmpH);
    {
        wxMemoryDC dc(bmp);
        dc.SetBackground(wxBrush(wxColour(60, 100, 160)));
        dc.Clear();
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(label, 4, 4);
    }

    dragImage_ = std::make_unique<wxGenericDragImage>(bmp, wxCursor(wxCURSOR_HAND));
    const wxPoint clientAnchor = frame_->ScreenToClient(screenAnchor);
    dragImage_->BeginDrag(wxPoint(0, 0), frame_, true);
    dragImage_->Show();
    dragImage_->Move(clientAnchor);
    draggingId_ = id;

    frame_->Bind(wxEVT_MOTION,  &UIManager::OnDragMotion,  this);
    frame_->Bind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);
}

void UIManager::OnDragMotion(wxMouseEvent& evt)
{
    if (dragImage_)
        dragImage_->Move(evt.GetPosition());
    evt.Skip();
}

void UIManager::OnDragRelease(wxMouseEvent& evt)
{
    if (dragImage_) {
        dragImage_->EndDrag();
        dragImage_.reset();
    }
    frame_->Unbind(wxEVT_MOTION,  &UIManager::OnDragMotion,  this);
    frame_->Unbind(wxEVT_LEFT_UP, &UIManager::OnDragRelease, this);

    const wxPoint screenPt = frame_->ClientToScreen(evt.GetPosition());
    auto* hit = wxFindWindowAtPoint(screenPt);
    auto* dstFrame = dynamic_cast<MainFrame*>(wxGetTopLevelParent(hit));

    if (dstFrame && dstFrame != frame_ && moveSessionCb_)
        moveSessionCb_(draggingId_, dstFrame);

    draggingId_ = 0;
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

void UIManager::UpdateStatusBar()
{
    if (activeId_ == 0 || sessions_.find(activeId_) == sessions_.end()) {
        frame_->SetStatusText("",    0);
        frame_->SetStatusText("Ready — use Connection > New Connection to start", 1);
        frame_->SetStatusText("",    2);
        frame_->SetStatusText("",    3);
        return;
    }

    const SessionUI* ui = FindSessionUI(activeId_);

    frame_->SetStatusText(wxString::Format("ID: %zu", activeId_), 0);
    frame_->SetStatusText(ui ? wxString::FromUTF8(ui->label) : wxString{}, 1);

    const DocLayout& layout = sm_.GetDocLayout(activeId_);
    const bool wrap = layout.GetWordWrap();
    frame_->SetStatusText(wrap ? "Wrap: ON" : "Wrap: OFF", 2);
    frame_->SyncWordWrapMenuItem(wrap);

    const CursorPos cur = layout.GetCursorDocPos();
    frame_->SetStatusText(
        wxString::Format("Ln %zu, Col %zu", cur.line + 1, cur.col + 1), 3);
}

void UIManager::SetupEditMenu(wxMenu* menu)
{
    menu->Append(kEditMenuCopy,      "Copy\tCtrl+Shift+C");
    menu->Append(kEditMenuPaste,     "Paste\tCtrl+Shift+V");
    menu->Append(kEditMenuSelectAll, "Select All\tCtrl+Shift+A");
    menu->AppendSeparator();
    menu->Append(kEditMenuPasteSel,  "Paste Selection");
    menu->Append(kEditMenuFind,      "Find in Terminal\tCtrl+Shift+F");
    menu->AppendSeparator();
    menu->Append(kEditMenuSaveFile,  "Save to File...");
    menu->Append(kEditMenuWebSearch, "Search the Web");

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

    const auto enableIfSelection = [this](wxUpdateUIEvent& e) {
        e.Enable(HasActiveSelection());
    };
    for (int id : {kEditMenuCopy, kEditMenuPasteSel,
                   kEditMenuSaveFile, kEditMenuWebSearch}) {
        frame_->Bind(wxEVT_UPDATE_UI, enableIfSelection, id);
    }
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
