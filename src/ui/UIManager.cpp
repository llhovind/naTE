#include "ui/UIManager.h"
#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include <wx/sizer.h>

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

    frame_->GetSizer()->Add(panel, 1, wxEXPAND);

    // Add menu item for this session.
    const int menuId = nextMenuId_++;
    connMenu_->Append(menuId, wxString::FromUTF8(label));
    frame_->Bind(wxEVT_MENU, [this, id](wxCommandEvent&) {
        RequestActivate(id);
    }, menuId);

    sessions_.emplace(id, SessionUI{ id, label, menuId, panel });

    // Auto-focus the new session.
    RequestActivate(id);

    frame_->Layout();
}

void UIManager::OnSessionTitleChanged(term::session::SessionId id, const std::string& title)
{
    // Arrives on the Session thread — cross to the UI thread before touching wx.
    frame_->CallAfter([this, id, title]() {
        SessionUI* ui = FindSessionUI(id);
        if (!ui) return;
        ui->label = title;
        connMenu_->SetLabel(ui->menuId, wxString::FromUTF8(title));
    });
}

void UIManager::OnSessionRefresh(term::session::SessionId id)
{
    // Arrives on the Session thread — must cross to the UI thread.
    frame_->CallAfter([this, id]() {
        SessionUI* ui = FindSessionUI(id);
        if (ui && ui->panel)
            ui->panel->OnDocumentUpdate();
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

    if (ui->panel) {
        ui->panel->Destroy();
        ui->panel = nullptr;
    }

    sessions_.erase(id);

    frame_->Layout();

    // If any sessions remain, activate the most recently inserted one.
    if (!sessions_.empty())
        RequestActivate(sessions_.begin()->first);
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
}

void UIManager::OnScroll(term::session::SessionId id, int topRow)
{
    sm_.OnScroll(id, topRow);
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

} // namespace ui
