#include "ui/UIManager.h"
#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include "ui/DocLayout.h"
#include "ui/SearchBar.h"
#include "ui/SearchController.h"
#include <wx/sizer.h>
#include <wx/string.h>

namespace ui {

UIManager::UIManager(term::session::SessionManager& sm,
                     wxMenu*                        connMenu,
                     MainFrame*                     frame,
                     const AppConfig&               cfg)
    : sm_(sm), connMenu_(connMenu), frame_(frame), cfg_(cfg)
{}

// ---------------------------------------------------------------------------
// ISessionObserver
// ---------------------------------------------------------------------------

void UIManager::OnSessionCreated(term::session::SessionId id, const std::string& label)
{
    // Create TerminalPanel as a child of the frame; wire all event callbacks.
    auto* panel = new TerminalPanel(frame_, cfg_);
    panel->SetDocLayout(&sm_.GetDocLayout(id));

    panel->SetScrollCallback([this, id](int topRow) {
        OnScroll(id, topRow);
    });
    panel->SetResizeCallback([this, id](unsigned short c, unsigned short r) {
        OnViewportResize(id, c, r);
    });
    panel->SetFocusCallback([this, id]() {
        RequestActivate(id);
    });

    // Create SearchController and SearchBar; bar is a wx child of panel (panel owns it).
    auto ctrl = std::make_unique<SearchController>(sm_.GetDocLayout(id), *panel);
    auto* bar = new SearchBar(panel, *ctrl);
    ctrl->SetBar(bar);
    panel->SetSearchBar(bar);

    frame_->GetSizer()->Add(panel, 1, wxEXPAND);

    // Add menu item for this session.
    const int menuId = nextMenuId_++;
    connMenu_->Append(menuId, wxString::FromUTF8(label));
    frame_->Bind(wxEVT_MENU, [this, id](wxCommandEvent&) {
        RequestActivate(id);
    }, menuId);

    SessionUI sui;
    sui.id        = id;
    sui.label     = label;
    sui.menuId    = menuId;
    sui.panel     = panel;
    sui.searchCtrl = std::move(ctrl);
    sessions_.emplace(id, std::move(sui));

    // Auto-focus the new session.
    RequestActivate(id);

    // On the first session, resize the frame so the panel receives exactly its
    // minimum client size — guaranteeing ViewportChars() == cfg.columns × cfg.rows.
    if (sessions_.size() == 1) {
        frame_->SetClientSize(panel->GetMinClientSize());
        frame_->Layout();
    }
}

void UIManager::OnSessionTitleChanged(term::session::SessionId id, const std::string& title)
{
    // Arrives on the Session thread — cross to the UI thread before touching wx.
    frame_->CallAfter([this, id, title]() {
        SessionUI* ui = FindSessionUI(id);
        if (!ui) return;
        ui->label = title;
        connMenu_->SetLabel(ui->menuId, wxString::FromUTF8(title));
        UpdateStatusBar();
    });
}

void UIManager::OnSessionRefresh(term::session::SessionId id)
{
    // Arrives on the Session thread — must cross to the UI thread.
    // Coalesce: if a refresh is already queued for this session, skip allocation.
    {
        std::lock_guard<std::mutex> lk(pendingRefreshMtx_);
        if (!pendingRefresh_.insert(id).second)
            return;
    }
    frame_->CallAfter([this, id]() {
        // Clear before processing so notifications arriving during the update
        // can immediately queue the next refresh.
        {
            std::lock_guard<std::mutex> lk(pendingRefreshMtx_);
            pendingRefresh_.erase(id);
        }
        SessionUI* ui = FindSessionUI(id);
        if (ui && ui->panel)
            ui->panel->OnDocumentUpdate();
        UpdateStatusBar();
    });
}

void UIManager::OnSessionDisconnected(term::session::SessionId id)
{
    // Arrives on the Session thread — must cross to the UI thread.
    frame_->CallAfter([this, id]() {
        sm_.CloseSession(id);
    });
}

void UIManager::OnSessionDestroyed(term::session::SessionId id)
{
    SessionUI* ui = FindSessionUI(id);
    if (!ui) return;

    connMenu_->Delete(ui->menuId);

    // Disconnect the SearchController from its bar before the panel (and bar) are destroyed.
    if (ui->searchCtrl) ui->searchCtrl->SetBar(nullptr);

    if (ui->panel) {
        ui->panel->Destroy();
        ui->panel = nullptr;
    }

    sessions_.erase(id);

    frame_->Layout();

    // If any sessions remain, activate the most recently inserted one;
    // RequestActivate calls UpdateStatusBar. Otherwise update directly to
    // reflect the no-session state.
    if (!sessions_.empty())
        RequestActivate(sessions_.begin()->first);
    else
        UpdateStatusBar();
}

// ---------------------------------------------------------------------------
// UI-thread event routing
// ---------------------------------------------------------------------------

void UIManager::RequestActivate(term::session::SessionId id)
{
    sm_.ActivateSession(id);

    // Show the activated panel; hide all others.
    for (auto& [sid, ui] : sessions_) {
        if (ui.panel)
            ui.panel->Show(sid == id);
    }
    frame_->Layout();
    UpdateStatusBar();
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
    SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    if (ui && ui->panel)
        ui->panel->EnsureCursorVisible();
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

SearchController* UIManager::GetActiveSearchController()
{
    SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    return ui ? ui->searchCtrl.get() : nullptr;
}

bool UIManager::SearchBarHasFocus() const
{
    const SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    if (!ui || !ui->panel) return false;
    return ui->panel->HasSearchBarFocus();
}

void UIManager::ShowSearchBarForActive(bool show)
{
    SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    if (ui && ui->panel)
        ui->panel->ShowSearchBar(show);
}

void UIManager::UpdateStatusBar()
{
    const term::session::SessionId activeId = sm_.GetActiveSessionId();

    if (activeId == 0 || sessions_.find(activeId) == sessions_.end()) {
        frame_->SetStatusText("",    0);
        frame_->SetStatusText("Ready — use Connection > New Connection to start", 1);
        frame_->SetStatusText("",    2);
        frame_->SetStatusText("",    3);
        return;
    }

    const SessionUI* ui = FindSessionUI(activeId);

    frame_->SetStatusText(wxString::Format("ID: %zu", activeId), 0);
    frame_->SetStatusText(ui ? wxString::FromUTF8(ui->label) : wxString{}, 1);

    const DocLayout& layout = sm_.GetDocLayout(activeId);
    const bool wrap = layout.GetWordWrap();
    frame_->SetStatusText(wrap ? "Wrap: ON" : "Wrap: OFF", 2);
    frame_->SyncWordWrapMenuItem(wrap);

    const CursorPos cur = layout.GetCursorDocPos();
    frame_->SetStatusText(
        wxString::Format("Ln %zu, Col %zu", cur.line + 1, cur.col + 1), 3);
}

} // namespace ui
