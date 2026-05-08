#include "ui/UIManager.h"
#include "ui/MainFrame.h"
#include "ui/TerminalPanel.h"
#include "ui/DocLayout.h"
#include "ui/SearchBar.h"
#include "ui/SearchController.h"
#include "ui/SelectionActions.h"
#include "ui/StringUtils.h"
#include <gtk/gtk.h>
#include <wx/sizer.h>
#include <wx/string.h>

namespace ui {

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

    SetupEditMenu(editMenu);
}

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
    panel->SetActionRegistry(selectionActions_.get());

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
    TerminalPanel* activePanel = nullptr;
    for (auto& [sid, ui] : sessions_) {
        const bool isActive = (sid == id);
        if (ui.panel) {
            ui.panel->Show(isActive);
            if (isActive) activePanel = ui.panel;
        }
    }
    frame_->Layout();
    UpdateStatusBar();
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

void UIManager::ShowSearchBarForActive(bool show, const std::u32string& initialQuery)
{
    SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    if (!ui || !ui->panel) return;
    ui->panel->ShowSearchBar(show);
    if (show && !initialQuery.empty() && ui->searchCtrl)
        ui->searchCtrl->SetInitialQuery(initialQuery);
}

std::u32string UIManager::GetActiveSelectedText() const
{
    const SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    if (!ui) return {};
    const std::u32string full = sm_.GetDocLayout(ui->id).GetSelectedText();
    // The per-line search doesn't support multi-line queries; use only the first line.
    const auto nl = full.find(U'\n');
    return (nl != std::u32string::npos) ? full.substr(0, nl) : full;
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

void UIManager::SetupEditMenu(wxMenu* menu)
{
    menu->Append(kEditMenuCopy,      "Copy");
    menu->Append(kEditMenuPaste,     "Paste");
    menu->Append(kEditMenuSelectAll, "Select All");
    menu->AppendSeparator();
    menu->Append(kEditMenuPasteSel,  "Paste Selection");
    menu->Append(kEditMenuFind,      "Find in Terminal");
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
        SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
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

void UIManager::PasteFromClipboard()
{
    gtk_clipboard_request_text(
        gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
        [](GtkClipboard*, const gchar* text, gpointer data) {
            if (!text) return;
            auto* self = static_cast<UIManager*>(data);
            self->router_.Paste(std::string(text));
            self->EnsureCursorVisibleForActive();
        },
        this);
}

bool UIManager::HasActiveSelection() const
{
    const SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    return ui && sm_.GetDocLayout(ui->id).HasSelection();
}

std::u32string UIManager::GetFullActiveSelectedText() const
{
    const SessionUI* ui = FindSessionUI(sm_.GetActiveSessionId());
    return ui ? sm_.GetDocLayout(ui->id).GetSelectedText() : std::u32string{};
}

} // namespace ui
